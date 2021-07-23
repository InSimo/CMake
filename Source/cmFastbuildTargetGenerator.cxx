/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#include "cmFastbuildTargetGenerator.h"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <map>
#include <ostream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <cm/memory>
#include <cm/string_view>
#include <cmext/algorithm>
#include <cmext/string_view>

#include <cm3p/json/value.h>
#include <cm3p/json/writer.h>

#include "cmComputeLinkInformation.h"
#include "cmCustomCommandGenerator.h"
#include "cmGeneratedFileStream.h"
#include "cmGeneratorExpression.h"
#include "cmGeneratorTarget.h"
#include "cmGlobalFastbuildGenerator.h"
#include "cmLocalGenerator.h"
#include "cmLocalFastbuildGenerator.h"
#include "cmMakefile.h"
#include "cmFastbuildNormalTargetGenerator.h"
#include "cmFastbuildUtilityTargetGenerator.h"
#include "cmOutputConverter.h"
#include "cmProperty.h"
#include "cmRange.h"
#include "cmRulePlaceholderExpander.h"
#include "cmSourceFile.h"
#include "cmStandardLevelResolver.h"
#include "cmState.h"
#include "cmStateTypes.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmake.h"

std::unique_ptr<cmFastbuildTargetGenerator> cmFastbuildTargetGenerator::New(
  cmGeneratorTarget* target)
{
  switch (target->GetType()) {
    case cmStateEnums::EXECUTABLE:
    case cmStateEnums::SHARED_LIBRARY:
    case cmStateEnums::STATIC_LIBRARY:
    case cmStateEnums::MODULE_LIBRARY:
    case cmStateEnums::OBJECT_LIBRARY:
      return cm::make_unique<cmFastbuildNormalTargetGenerator>(target);

    case cmStateEnums::UTILITY:
    case cmStateEnums::INTERFACE_LIBRARY:
    case cmStateEnums::GLOBAL_TARGET:
      return cm::make_unique<cmFastbuildUtilityTargetGenerator>(target);

    default:
      return std::unique_ptr<cmFastbuildTargetGenerator>();
  }
}

cmFastbuildTargetGenerator::cmFastbuildTargetGenerator(cmGeneratorTarget* target)
  : cmCommonTargetGenerator(target)
  , OSXBundleGenerator(nullptr)
  , LocalGenerator(
      static_cast<cmLocalFastbuildGenerator*>(target->GetLocalGenerator()))
{
  for (auto const& fileConfig :
       target->Makefile->GetGeneratorConfigs(cmMakefile::IncludeEmptyConfig)) {
    this->Configs[fileConfig].MacOSXContentGenerator =
      cm::make_unique<MacOSXContentGeneratorType>(this, fileConfig);
  }
}

cmFastbuildTargetGenerator::~cmFastbuildTargetGenerator() = default;

cmGeneratedFileStream& cmFastbuildTargetGenerator::GetImplFileStream(
  const std::string& config) const
{
  return *this->GetGlobalGenerator()->GetImplFileStream(config);
}

cmGeneratedFileStream& cmFastbuildTargetGenerator::GetCommonFileStream() const
{
  return *this->GetGlobalGenerator()->GetCommonFileStream();
}

cmGeneratedFileStream& cmFastbuildTargetGenerator::GetRulesFileStream() const
{
  return *this->GetGlobalGenerator()->GetRulesFileStream();
}

cmGlobalFastbuildGenerator* cmFastbuildTargetGenerator::GetGlobalGenerator() const
{
  return this->LocalGenerator->GetGlobalFastbuildGenerator();
}

std::string cmFastbuildTargetGenerator::LanguageCompilerRule(
  const std::string& lang, const std::string& config) const
{
  return cmStrCat(
    lang, "_COMPILER__",
    cmGlobalFastbuildGenerator::EncodeRuleName(this->GeneratorTarget->GetName()),
    '_', config);
}

std::string cmFastbuildTargetGenerator::LanguagePreprocessAndScanRule(
  std::string const& lang, const std::string& config) const
{
  return cmStrCat(
    lang, "_PREPROCESS_SCAN__",
    cmGlobalFastbuildGenerator::EncodeRuleName(this->GeneratorTarget->GetName()),
    '_', config);
}

std::string cmFastbuildTargetGenerator::LanguageScanRule(
  std::string const& lang, const std::string& config) const
{
  return cmStrCat(
    lang, "_SCAN__",
    cmGlobalFastbuildGenerator::EncodeRuleName(this->GeneratorTarget->GetName()),
    '_', config);
}

bool cmFastbuildTargetGenerator::NeedExplicitPreprocessing(
  std::string const& lang) const
{
  return lang == "Fortran";
}

bool cmFastbuildTargetGenerator::CompileWithDefines(std::string const& lang) const
{
  return this->Makefile->IsOn(
    cmStrCat("CMAKE_", lang, "_COMPILE_WITH_DEFINES"));
}

std::string cmFastbuildTargetGenerator::LanguageDyndepRule(
  const std::string& lang, const std::string& config) const
{
  return cmStrCat(
    lang, "_DYNDEP__",
    cmGlobalFastbuildGenerator::EncodeRuleName(this->GeneratorTarget->GetName()),
    '_', config);
}

bool cmFastbuildTargetGenerator::NeedCxxModuleSupport(
  std::string const& lang, std::string const& config) const
{
  if (lang != "CXX") {
    return false;
  }
  if (!this->Makefile->IsOn("CMAKE_EXPERIMENTAL_CXX_MODULE_DYNDEP")) {
    return false;
  }
  cmGeneratorTarget const* tgt = this->GetGeneratorTarget();
  cmStandardLevelResolver standardResolver(this->Makefile);
  bool const uses_cxx20 =
    standardResolver.HaveStandardAvailable(tgt, "CXX", config, "cxx_std_20");
  return uses_cxx20 && this->GetGlobalGenerator()->CheckCxxModuleSupport();
}

bool cmFastbuildTargetGenerator::NeedDyndep(std::string const& lang,
                                        std::string const& config) const
{
  return lang == "Fortran" || this->NeedCxxModuleSupport(lang, config);
}

std::string cmFastbuildTargetGenerator::OrderDependsTargetForTarget(
  const std::string& config)
{
  return this->GetGlobalGenerator()->OrderDependsTargetForTarget(
    this->GeneratorTarget, config);
}

// TODO: Most of the code is picked up from
// void cmMakefileExecutableTargetGenerator::WriteExecutableRule(bool relink),
// void cmMakefileTargetGenerator::WriteTargetLanguageFlags()
// Refactor it.
std::string cmFastbuildTargetGenerator::ComputeFlagsForObject(
  cmSourceFile const* source, const std::string& language,
  const std::string& config)
{
  std::vector<std::string> architectures;
  std::unordered_map<std::string, std::string> pchSources;
  this->GeneratorTarget->GetAppleArchs(config, architectures);
  if (architectures.empty()) {
    architectures.emplace_back();
  }

  std::string filterArch;
  for (const std::string& arch : architectures) {
    const std::string pchSource =
      this->GeneratorTarget->GetPchSource(config, language, arch);
    if (pchSource == source->GetFullPath()) {
      filterArch = arch;
    }
    if (!pchSource.empty()) {
      pchSources.insert(std::make_pair(pchSource, arch));
    }
  }

  std::string flags;
  // Explicitly add the explicit language flag before any other flag
  // so user flags can override it.
  this->GeneratorTarget->AddExplicitLanguageFlags(flags, *source);

  if (!flags.empty()) {
    flags += " ";
  }
  flags += this->GetFlags(language, config, filterArch);

  // Add Fortran format flags.
  if (language == "Fortran") {
    this->AppendFortranFormatFlags(flags, *source);
    this->AppendFortranPreprocessFlags(flags, *source);
  }

  // Add source file specific flags.
  cmGeneratorExpressionInterpreter genexInterpreter(
    this->LocalGenerator, config, this->GeneratorTarget, language);

  const std::string COMPILE_FLAGS("COMPILE_FLAGS");
  if (cmProp cflags = source->GetProperty(COMPILE_FLAGS)) {
    this->LocalGenerator->AppendFlags(
      flags, genexInterpreter.Evaluate(*cflags, COMPILE_FLAGS));
  }

  const std::string COMPILE_OPTIONS("COMPILE_OPTIONS");
  if (cmProp coptions = source->GetProperty(COMPILE_OPTIONS)) {
    this->LocalGenerator->AppendCompileOptions(
      flags, genexInterpreter.Evaluate(*coptions, COMPILE_OPTIONS));
  }

  // Add precompile headers compile options.
  if (!pchSources.empty() && !source->GetProperty("SKIP_PRECOMPILE_HEADERS")) {
    std::string pchOptions;
    auto pchIt = pchSources.find(source->GetFullPath());
    if (pchIt != pchSources.end()) {
      pchOptions = this->GeneratorTarget->GetPchCreateCompileOptions(
        config, language, pchIt->second);
    } else {
      pchOptions =
        this->GeneratorTarget->GetPchUseCompileOptions(config, language);
    }

    this->LocalGenerator->AppendCompileOptions(
      flags, genexInterpreter.Evaluate(pchOptions, COMPILE_OPTIONS));
  }

  return flags;
}

void cmFastbuildTargetGenerator::AddIncludeFlags(std::string& languageFlags,
                                             std::string const& language,
                                             const std::string& config)
{
  std::vector<std::string> includes;
  this->LocalGenerator->GetIncludeDirectories(includes, this->GeneratorTarget,
                                              language, config);
  // Add include directory flags.
  std::string includeFlags = this->LocalGenerator->GetIncludeFlags(
    includes, this->GeneratorTarget, language,
    language == "RC", // full include paths for RC needed by cmcldeps
    false, config);
  if (this->GetGlobalGenerator()->IsGCCOnWindows()) {
    std::replace(includeFlags.begin(), includeFlags.end(), '\\', '/');
  }

  this->LocalGenerator->AppendFlags(languageFlags, includeFlags);
}

// TODO: Refactor with
// void cmMakefileTargetGenerator::WriteTargetLanguageFlags().
std::string cmFastbuildTargetGenerator::ComputeDefines(cmSourceFile const* source,
                                                   const std::string& language,
                                                   const std::string& config)
{
  std::set<std::string> defines;
  cmGeneratorExpressionInterpreter genexInterpreter(
    this->LocalGenerator, config, this->GeneratorTarget, language);

  // Seriously??
  if (this->GetGlobalGenerator()->IsMultiConfig()) {
    defines.insert(cmStrCat("CMAKE_INTDIR=\"", config, '"'));
  }

  const std::string COMPILE_DEFINITIONS("COMPILE_DEFINITIONS");
  if (cmProp compile_defs = source->GetProperty(COMPILE_DEFINITIONS)) {
    this->LocalGenerator->AppendDefines(
      defines, genexInterpreter.Evaluate(*compile_defs, COMPILE_DEFINITIONS));
  }

  std::string defPropName =
    cmStrCat("COMPILE_DEFINITIONS_", cmSystemTools::UpperCase(config));
  if (cmProp config_compile_defs = source->GetProperty(defPropName)) {
    this->LocalGenerator->AppendDefines(
      defines,
      genexInterpreter.Evaluate(*config_compile_defs, COMPILE_DEFINITIONS));
  }

  std::string definesString = this->GetDefines(language, config);
  this->LocalGenerator->JoinDefines(defines, definesString, language);

  return definesString;
}

std::string cmFastbuildTargetGenerator::ComputeIncludes(
  cmSourceFile const* source, const std::string& language,
  const std::string& config)
{
  std::vector<std::string> includes;
  cmGeneratorExpressionInterpreter genexInterpreter(
    this->LocalGenerator, config, this->GeneratorTarget, language);

  const std::string INCLUDE_DIRECTORIES("INCLUDE_DIRECTORIES");
  if (cmProp cincludes = source->GetProperty(INCLUDE_DIRECTORIES)) {
    this->LocalGenerator->AppendIncludeDirectories(
      includes, genexInterpreter.Evaluate(*cincludes, INCLUDE_DIRECTORIES),
      *source);
  }

  std::string includesString = this->LocalGenerator->GetIncludeFlags(
    includes, this->GeneratorTarget, language, true, false, config);
  this->LocalGenerator->AppendFlags(includesString,
                                    this->GetIncludes(language, config));

  return includesString;
}

cmFastbuildDeps cmFastbuildTargetGenerator::ComputeLinkDeps(
  const std::string& linkLanguage, const std::string& config,
  bool ignoreType) const
{
  // Static libraries never depend on other targets for linking.
  if (!ignoreType &&
      (this->GeneratorTarget->GetType() == cmStateEnums::STATIC_LIBRARY ||
       this->GeneratorTarget->GetType() == cmStateEnums::OBJECT_LIBRARY)) {
    return cmFastbuildDeps();
  }

  cmComputeLinkInformation* cli =
    this->GeneratorTarget->GetLinkInformation(config);
  if (!cli) {
    return cmFastbuildDeps();
  }

  const std::vector<std::string>& deps = cli->GetDepends();
  cmFastbuildDeps result(deps.size());
  std::transform(deps.begin(), deps.end(), result.begin(),
                 this->MapToFastbuildPath());

  // Add a dependency on the link definitions file, if any.
  if (cmGeneratorTarget::ModuleDefinitionInfo const* mdi =
        this->GeneratorTarget->GetModuleDefinitionInfo(config)) {
    for (cmSourceFile const* src : mdi->Sources) {
      result.push_back(this->ConvertToFastbuildPath(src->GetFullPath()));
    }
  }

  // Add a dependency on user-specified manifest files, if any.
  std::vector<cmSourceFile const*> manifest_srcs;
  this->GeneratorTarget->GetManifests(manifest_srcs, config);
  for (cmSourceFile const* manifest_src : manifest_srcs) {
    result.push_back(this->ConvertToFastbuildPath(manifest_src->GetFullPath()));
  }

  // Add user-specified dependencies.
  std::vector<std::string> linkDeps;
  this->GeneratorTarget->GetLinkDepends(linkDeps, config, linkLanguage);
  std::transform(linkDeps.begin(), linkDeps.end(), std::back_inserter(result),
                 this->MapToFastbuildPath());

  return result;
}

std::string cmFastbuildTargetGenerator::GetSourceFilePath(
  cmSourceFile const* source) const
{
  return this->ConvertToFastbuildPath(source->GetFullPath());
}

std::string cmFastbuildTargetGenerator::GetObjectFilePath(
  cmSourceFile const* source, const std::string& config) const
{
  std::string path = this->LocalGenerator->GetHomeRelativeOutputPath();
  if (!path.empty()) {
    path += '/';
  }
  std::string const& objectName = this->GeneratorTarget->GetObjectName(source);
  path += cmStrCat(
    this->LocalGenerator->GetTargetDirectory(this->GeneratorTarget),
    this->GetGlobalGenerator()->ConfigDirectory(config), '/', objectName);
  return path;
}

std::string cmFastbuildTargetGenerator::GetPreprocessedFilePath(
  cmSourceFile const* source, const std::string& config) const
{
  // Choose an extension to compile already-preprocessed source.
  std::string ppExt = source->GetExtension();
  if (cmHasLiteralPrefix(ppExt, "F")) {
    // Some Fortran compilers automatically enable preprocessing for
    // upper-case extensions.  Since the source is already preprocessed,
    // use a lower-case extension.
    ppExt = cmSystemTools::LowerCase(ppExt);
  }
  if (ppExt == "fpp") {
    // Some Fortran compilers automatically enable preprocessing for
    // the ".fpp" extension.  Since the source is already preprocessed,
    // use the ".f" extension.
    ppExt = "f";
  }

  // Take the object file name and replace the extension.
  std::string const& objName = this->GeneratorTarget->GetObjectName(source);
  std::string const& objExt =
    this->GetGlobalGenerator()->GetLanguageOutputExtension(*source);
  assert(objName.size() >= objExt.size());
  std::string const ppName =
    cmStrCat(objName.substr(0, objName.size() - objExt.size()), "-pp.", ppExt);

  std::string path = this->LocalGenerator->GetHomeRelativeOutputPath();
  if (!path.empty()) {
    path += '/';
  }
  path +=
    cmStrCat(this->LocalGenerator->GetTargetDirectory(this->GeneratorTarget),
             this->GetGlobalGenerator()->ConfigDirectory(config), '/', ppName);
  return path;
}

std::string cmFastbuildTargetGenerator::GetDyndepFilePath(
  std::string const& lang, const std::string& config) const
{
  std::string path = this->LocalGenerator->GetHomeRelativeOutputPath();
  if (!path.empty()) {
    path += '/';
  }
  path += cmStrCat(
    this->LocalGenerator->GetTargetDirectory(this->GeneratorTarget),
    this->GetGlobalGenerator()->ConfigDirectory(config), '/', lang, ".dd");
  return path;
}

std::string cmFastbuildTargetGenerator::GetTargetDependInfoPath(
  std::string const& lang, const std::string& config) const
{
  std::string path =
    cmStrCat(this->Makefile->GetCurrentBinaryDirectory(), '/',
             this->LocalGenerator->GetTargetDirectory(this->GeneratorTarget),
             this->GetGlobalGenerator()->ConfigDirectory(config), '/', lang,
             "DependInfo.json");
  return path;
}

std::string cmFastbuildTargetGenerator::GetTargetOutputDir(
  const std::string& config) const
{
  std::string dir = this->GeneratorTarget->GetDirectory(config);
  return this->ConvertToFastbuildPath(dir);
}

std::string cmFastbuildTargetGenerator::GetTargetFilePath(
  const std::string& name, const std::string& config) const
{
  std::string path = this->GetTargetOutputDir(config);
  if (path.empty() || path == ".") {
    return name;
  }
  path += cmStrCat('/', name);
  return path;
}

std::string cmFastbuildTargetGenerator::GetTargetName() const
{
  return this->GeneratorTarget->GetName();
}

bool cmFastbuildTargetGenerator::SetMsvcTargetPdbVariable(
  cmFastbuildVars& vars, const std::string& config) const
{
  cmMakefile* mf = this->GetMakefile();
  if (mf->GetDefinition("MSVC_C_ARCHITECTURE_ID") ||
      mf->GetDefinition("MSVC_CXX_ARCHITECTURE_ID") ||
      mf->GetDefinition("MSVC_CUDA_ARCHITECTURE_ID")) {
    std::string pdbPath;
    std::string compilePdbPath = this->ComputeTargetCompilePDB(config);
    if (this->GeneratorTarget->GetType() == cmStateEnums::EXECUTABLE ||
        this->GeneratorTarget->GetType() == cmStateEnums::STATIC_LIBRARY ||
        this->GeneratorTarget->GetType() == cmStateEnums::SHARED_LIBRARY ||
        this->GeneratorTarget->GetType() == cmStateEnums::MODULE_LIBRARY) {
      pdbPath = cmStrCat(this->GeneratorTarget->GetPDBDirectory(config), '/',
                         this->GeneratorTarget->GetPDBName(config));
    }

    vars["TARGET_PDB"] = this->GetLocalGenerator()->ConvertToOutputFormat(
      this->ConvertToFastbuildPath(pdbPath), cmOutputConverter::SHELL);
    vars["TARGET_COMPILE_PDB"] =
      this->GetLocalGenerator()->ConvertToOutputFormat(
        this->ConvertToFastbuildPath(compilePdbPath), cmOutputConverter::SHELL);

    this->EnsureParentDirectoryExists(pdbPath);
    this->EnsureParentDirectoryExists(compilePdbPath);
    return true;
  }
  return false;
}

void cmFastbuildTargetGenerator::WriteLanguageRules(const std::string& language,
                                                const std::string& config)
{
#ifdef FASTBUILD_GEN_VERBOSE_FILES
  this->GetRulesFileStream() << "// Rules for language " << language << "\n\n";
#endif
  this->WriteCompileRule(language, config);
}

namespace {
// Create the command to run the dependency scanner
std::string GetScanCommand(const std::string& cmakeCmd, const std::string& tdi,
                           const std::string& lang, const std::string& ppFile,
                           const std::string& ddiFile)
{
  return cmStrCat(cmakeCmd, " -E cmake_fastbuild_depends --tdi=", tdi,
                  " --lang=", lang, " --src=$in", " --pp=", ppFile,
                  " --dep=$DEP_FILE --obj=$OBJ_FILE --ddi=", ddiFile);
}

// Helper function to create dependency scanning rule that may or may
// not perform explicit preprocessing too.
cmFastbuildRule GetScanRule(
  const std::string& ruleName,
  cmRulePlaceholderExpander::RuleVariables const& vars,
  const std::string& responseFlag, const std::string& flags,
  cmRulePlaceholderExpander* const rulePlaceholderExpander,
  cmLocalFastbuildGenerator* generator, std::vector<std::string> scanCmds,
  const std::string& outputConfig)
{
  cmFastbuildRule rule(ruleName);
  // Scanning always uses a depfile for preprocessor dependencies.
  rule.DepType = ""; // no deps= for multiple outputs
  rule.DepFile = "$DEP_FILE";

  cmRulePlaceholderExpander::RuleVariables scanVars;
  scanVars.CMTargetName = vars.CMTargetName;
  scanVars.CMTargetType = vars.CMTargetType;
  scanVars.Language = vars.Language;
  scanVars.Object = "$OBJ_FILE";
  scanVars.PreprocessedSource = "$out";
  scanVars.DynDepFile = "$DYNDEP_INTERMEDIATE_FILE";
  scanVars.DependencyFile = rule.DepFile.c_str();
  scanVars.DependencyTarget = "$out";

  // Scanning needs the same preprocessor settings as direct compilation would.
  scanVars.Source = vars.Source;
  scanVars.Defines = vars.Defines;
  scanVars.Includes = vars.Includes;

  // Scanning needs the compilation flags too.
  std::string scanFlags = flags;

  // If using a response file, move defines, includes, and flags into it.
  if (!responseFlag.empty()) {
    rule.RspFile = "$RSP_FILE";
    rule.RspContent =
      cmStrCat(' ', scanVars.Defines, ' ', scanVars.Includes, ' ', scanFlags);
    scanFlags = cmStrCat(responseFlag, rule.RspFile);
    scanVars.Defines = "";
    scanVars.Includes = "";
  }

  scanVars.Flags = scanFlags.c_str();

  // Rule for scanning a source file.
  for (std::string& scanCmd : scanCmds) {
    rulePlaceholderExpander->ExpandRuleVariables(generator, scanCmd, scanVars);
  }
  rule.Command =
    generator->BuildCommandLine(scanCmds, outputConfig, outputConfig);

  return rule;
}
}

void cmFastbuildTargetGenerator::WriteCompileRule(const std::string& lang,
                                              const std::string& config)
{
  cmRulePlaceholderExpander::RuleVariables vars;
  vars.CMTargetName = this->GetGeneratorTarget()->GetName().c_str();
  vars.CMTargetType =
    cmState::GetTargetTypeName(this->GetGeneratorTarget()->GetType()).c_str();
  vars.Language = lang.c_str();
  vars.Source = "$in";
  vars.Object = "$out";
  vars.Defines = "$DEFINES";
  vars.Includes = "$INCLUDES";
  vars.TargetPDB = "$TARGET_PDB";
  vars.TargetCompilePDB = "$TARGET_COMPILE_PDB";
  vars.ObjectDir = "$OBJECT_DIR";
  vars.ObjectFileDir = "$OBJECT_FILE_DIR";
  vars.ISPCHeader = "$ISPC_HEADER_FILE";

  cmMakefile* mf = this->GetMakefile();

  // For some cases we scan to dynamically discover dependencies.
  bool const needDyndep = this->NeedDyndep(lang, config);
  bool const compilationPreprocesses = !this->NeedExplicitPreprocessing(lang);

  std::string flags = "$FLAGS";

  std::string responseFlag;
  bool const lang_supports_response = lang != "RC";
  if (lang_supports_response && this->ForceResponseFile()) {
    std::string const responseFlagVar =
      cmStrCat("CMAKE_", lang, "_RESPONSE_FILE_FLAG");
    responseFlag = this->Makefile->GetSafeDefinition(responseFlagVar);
    if (responseFlag.empty() && lang != "CUDA") {
      responseFlag = "@";
    }
  }
  std::string const modmapFormatVar =
    cmStrCat("CMAKE_EXPERIMENTAL_", lang, "_MODULE_MAP_FORMAT");
  std::string const modmapFormat =
    this->Makefile->GetSafeDefinition(modmapFormatVar);

  std::unique_ptr<cmRulePlaceholderExpander> rulePlaceholderExpander(
    this->GetLocalGenerator()->CreateRulePlaceholderExpander());

  std::string const tdi = this->GetLocalGenerator()->ConvertToOutputFormat(
    this->ConvertToFastbuildPath(this->GetTargetDependInfoPath(lang, config)),
    cmLocalGenerator::SHELL);

  std::string launcher;
  cmProp val = this->GetLocalGenerator()->GetRuleLauncher(
    this->GetGeneratorTarget(), "RULE_LAUNCH_COMPILE");
  if (cmNonempty(val)) {
    launcher = cmStrCat(*val, ' ');
  }

  std::string const cmakeCmd =
    this->GetLocalGenerator()->ConvertToOutputFormat(
      cmSystemTools::GetCMakeCommand(), cmLocalGenerator::SHELL);

  if (needDyndep) {
    // Rule to scan dependencies of sources that need preprocessing.
    {
      std::vector<std::string> scanCommands;
      std::string scanRuleName;
      if (compilationPreprocesses) {
        scanRuleName = this->LanguageScanRule(lang, config);
        std::string const& scanCommand = mf->GetRequiredDefinition(
          cmStrCat("CMAKE_EXPERIMENTAL_", lang, "_SCANDEP_SOURCE"));
        cmExpandList(scanCommand, scanCommands);
        for (std::string& i : scanCommands) {
          i = cmStrCat(launcher, i);
        }
      } else {
        scanRuleName = this->LanguagePreprocessAndScanRule(lang, config);
        std::string const& ppCommmand = mf->GetRequiredDefinition(
          cmStrCat("CMAKE_", lang, "_PREPROCESS_SOURCE"));
        cmExpandList(ppCommmand, scanCommands);
        for (std::string& i : scanCommands) {
          i = cmStrCat(launcher, i);
        }
        scanCommands.emplace_back(GetScanCommand(cmakeCmd, tdi, lang, "$out",
                                                 "$DYNDEP_INTERMEDIATE_FILE"));
      }

      auto scanRule = GetScanRule(
        scanRuleName, vars, responseFlag, flags, rulePlaceholderExpander.get(),
        this->GetLocalGenerator(), std::move(scanCommands), config);

      scanRule.Comment =
        cmStrCat("Rule for generating ", lang, " dependencies.");
      if (compilationPreprocesses) {
        scanRule.Description =
          cmStrCat("Scanning $in for ", lang, " dependencies");
      } else {
        scanRule.Description =
          cmStrCat("Building ", lang, " preprocessed $out");
      }

      this->GetGlobalGenerator()->AddRule(scanRule);
    }

    if (!compilationPreprocesses) {
      // Compilation will not preprocess, so it does not need the defines
      // unless the compiler wants them for some other purpose.
      if (!this->CompileWithDefines(lang)) {
        vars.Defines = "";
      }

      // Rule to scan dependencies of sources that do not need preprocessing.
      std::string const& scanRuleName = this->LanguageScanRule(lang, config);
      std::vector<std::string> scanCommands;
      scanCommands.emplace_back(
        GetScanCommand(cmakeCmd, tdi, lang, "$in", "$out"));

      auto scanRule = GetScanRule(
        scanRuleName, vars, "", flags, rulePlaceholderExpander.get(),
        this->GetLocalGenerator(), std::move(scanCommands), config);

      // Write the rule for generating dependencies for the given language.
      scanRule.Comment = cmStrCat("Rule for generating ", lang,
                                  " dependencies on non-preprocessed files.");
      scanRule.Description =
        cmStrCat("Generating ", lang, " dependencies for $in");

      this->GetGlobalGenerator()->AddRule(scanRule);
    }

    // Write the rule for ninja dyndep file generation.
    cmFastbuildRule rule(this->LanguageDyndepRule(lang, config));
    // Command line length is almost always limited -> use response file for
    // dyndep rules
    rule.RspFile = "$out.rsp";
    rule.RspContent = "$in";

    // Run CMake dependency scanner on the source file (using the preprocessed
    // source if that was performed).
    std::string ddModmapArg;
    if (!modmapFormat.empty()) {
      ddModmapArg += cmStrCat(" --modmapfmt=", modmapFormat);
    }
    {
      std::vector<std::string> ddCmds;
      {
        std::string ccmd = cmStrCat(
          cmakeCmd, " -E cmake_fastbuild_dyndep --tdi=", tdi, " --lang=", lang,
          ddModmapArg, " --dd=$out @", rule.RspFile);
        ddCmds.emplace_back(std::move(ccmd));
      }
      rule.Command =
        this->GetLocalGenerator()->BuildCommandLine(ddCmds, config, config);
    }
    rule.Comment =
      cmStrCat("Rule to generate fastbuild dyndep files for ", lang, '.');
    rule.Description = cmStrCat("Generating ", lang, " dyndep file $out");
    this->GetGlobalGenerator()->AddRule(rule);
  }

  cmFastbuildRule rule(this->LanguageCompilerRule(lang, config));
  // If using a response file, move defines, includes, and flags into it.
  if (!responseFlag.empty()) {
    rule.RspFile = "$RSP_FILE";
    rule.RspContent =
      cmStrCat(' ', vars.Defines, ' ', vars.Includes, ' ', flags);
    flags = cmStrCat(responseFlag, rule.RspFile);
    vars.Defines = "";
    vars.Includes = "";
  }

  // Tell ninja dependency format so all deps can be loaded into a database
  std::string cldeps;
  if (!compilationPreprocesses) {
    // The compiler will not do preprocessing, so it has no such dependencies.
  } else if (mf->IsOn(cmStrCat("CMAKE_FASTBUILD_CMCLDEPS_", lang))) {
    // For the MS resource compiler we need cmcldeps, but skip dependencies
    // for source-file try_compile cases because they are always fresh.
    if (!mf->GetIsSourceFileTryCompile()) {
      rule.DepType = "gcc";
      rule.DepFile = "$DEP_FILE";
      cmProp d = mf->GetDefinition("CMAKE_C_COMPILER");
      const std::string cl =
        d ? *d : mf->GetSafeDefinition("CMAKE_CXX_COMPILER");
      cldeps = cmStrCat('"', cmSystemTools::GetCMClDepsCommand(), "\" ", lang,
                        ' ', vars.Source, " $DEP_FILE $out \"",
                        mf->GetSafeDefinition("CMAKE_CL_SHOWINCLUDES_PREFIX"),
                        "\" \"", cl, "\" ");
    }
  } else {
    const auto& depType = this->GetMakefile()->GetSafeDefinition(
      cmStrCat("CMAKE_", lang, "_DEPFILE_FORMAT"));
    if (depType == "msvc"_s) {
      rule.DepType = "msvc";
      rule.DepFile.clear();
    } else {
      rule.DepType = "gcc";
      rule.DepFile = "$DEP_FILE";
    }
    vars.DependencyFile = rule.DepFile.c_str();
    vars.DependencyTarget = "$out";

    const std::string flagsName = cmStrCat("CMAKE_DEPFILE_FLAGS_", lang);
    std::string depfileFlags = mf->GetSafeDefinition(flagsName);
    if (!depfileFlags.empty()) {
      rulePlaceholderExpander->ExpandRuleVariables(this->GetLocalGenerator(),
                                                   depfileFlags, vars);
      flags += cmStrCat(' ', depfileFlags);
    }
  }

  if (needDyndep && !modmapFormat.empty()) {
    std::string modmapFlags = mf->GetRequiredDefinition(
      cmStrCat("CMAKE_EXPERIMENTAL_", lang, "_MODULE_MAP_FLAG"));
    cmSystemTools::ReplaceString(modmapFlags, "<MODULE_MAP_FILE>",
                                 "$DYNDEP_MODULE_MAP_FILE");
    flags += cmStrCat(' ', modmapFlags);
  }

  vars.Flags = flags.c_str();
  vars.DependencyFile = rule.DepFile.c_str();

  // Rule for compiling object file.
  std::vector<std::string> compileCmds;
  if (lang == "CUDA") {
    std::string cmdVar;
    if (this->GeneratorTarget->GetPropertyAsBool(
          "CUDA_SEPARABLE_COMPILATION")) {
      cmdVar = "CMAKE_CUDA_COMPILE_SEPARABLE_COMPILATION";
    } else if (this->GeneratorTarget->GetPropertyAsBool(
                 "CUDA_PTX_COMPILATION")) {
      cmdVar = "CMAKE_CUDA_COMPILE_PTX_COMPILATION";
    } else {
      cmdVar = "CMAKE_CUDA_COMPILE_WHOLE_COMPILATION";
    }
    const std::string& compileCmd = mf->GetRequiredDefinition(cmdVar);
    cmExpandList(compileCmd, compileCmds);
  } else {
    const std::string cmdVar = cmStrCat("CMAKE_", lang, "_COMPILE_OBJECT");
    const std::string& compileCmd = mf->GetRequiredDefinition(cmdVar);
    cmExpandList(compileCmd, compileCmds);
  }

  // See if we need to use a compiler launcher like ccache or distcc
  std::string compilerLauncher;
  if (!compileCmds.empty() &&
      (lang == "C" || lang == "CXX" || lang == "Fortran" || lang == "CUDA" ||
       lang == "ISPC" || lang == "OBJC" || lang == "OBJCXX")) {
    std::string const clauncher_prop = cmStrCat(lang, "_COMPILER_LAUNCHER");
    cmProp clauncher = this->GeneratorTarget->GetProperty(clauncher_prop);
    if (cmNonempty(clauncher)) {
      compilerLauncher = *clauncher;
    }
  }

  // Maybe insert an include-what-you-use runner.
  if (!compileCmds.empty() &&
      (lang == "C" || lang == "CXX" || lang == "OBJC" || lang == "OBJCXX")) {
    std::string const tidy_prop = cmStrCat(lang, "_CLANG_TIDY");
    cmProp tidy = this->GeneratorTarget->GetProperty(tidy_prop);
    cmProp iwyu = nullptr;
    cmProp cpplint = nullptr;
    cmProp cppcheck = nullptr;
    if (lang == "C" || lang == "CXX") {
      std::string const iwyu_prop = cmStrCat(lang, "_INCLUDE_WHAT_YOU_USE");
      iwyu = this->GeneratorTarget->GetProperty(iwyu_prop);
      std::string const cpplint_prop = cmStrCat(lang, "_CPPLINT");
      cpplint = this->GeneratorTarget->GetProperty(cpplint_prop);
      std::string const cppcheck_prop = cmStrCat(lang, "_CPPCHECK");
      cppcheck = this->GeneratorTarget->GetProperty(cppcheck_prop);
    }
    if (cmNonempty(iwyu) || cmNonempty(tidy) || cmNonempty(cpplint) ||
        cmNonempty(cppcheck)) {
      std::string run_iwyu = cmStrCat(cmakeCmd, " -E __run_co_compile");
      if (!compilerLauncher.empty()) {
        // In __run_co_compile case the launcher command is supplied
        // via --launcher=<maybe-list> and consumed
        run_iwyu +=
          cmStrCat(" --launcher=",
                   this->LocalGenerator->EscapeForShell(compilerLauncher));
        compilerLauncher.clear();
      }
      if (cmNonempty(iwyu)) {
        run_iwyu += cmStrCat(" --iwyu=",
                             this->GetLocalGenerator()->EscapeForShell(*iwyu));
      }
      if (cmNonempty(tidy)) {
        run_iwyu += " --tidy=";
        cmProp p = this->Makefile->GetDefinition(
          cmStrCat("CMAKE_", lang, "_CLANG_TIDY_DRIVER_MODE"));
        std::string driverMode;
        if (cmNonempty(p)) {
          driverMode = *p;
        } else {
          driverMode = lang == "C" ? "gcc" : "g++";
        }
        run_iwyu += this->GetLocalGenerator()->EscapeForShell(
          cmStrCat(*tidy, ";--extra-arg-before=--driver-mode=", driverMode));
      }
      if (cmNonempty(cpplint)) {
        run_iwyu += cmStrCat(
          " --cpplint=", this->GetLocalGenerator()->EscapeForShell(*cpplint));
      }
      if (cmNonempty(cppcheck)) {
        run_iwyu +=
          cmStrCat(" --cppcheck=",
                   this->GetLocalGenerator()->EscapeForShell(*cppcheck));
      }
      if (cmNonempty(tidy) || cmNonempty(cpplint) || cmNonempty(cppcheck)) {
        run_iwyu += " --source=$in";
      }
      run_iwyu += " -- ";
      compileCmds.front().insert(0, run_iwyu);
    }
  }

  // If compiler launcher was specified and not consumed above, it
  // goes to the beginning of the command line.
  if (!compileCmds.empty() && !compilerLauncher.empty()) {
    std::vector<std::string> args = cmExpandedList(compilerLauncher, true);
    if (!args.empty()) {
      args[0] = this->LocalGenerator->ConvertToOutputFormat(
        args[0], cmOutputConverter::SHELL);
      for (std::string& i : cmMakeRange(args.begin() + 1, args.end())) {
        i = this->LocalGenerator->EscapeForShell(i);
      }
    }
    compileCmds.front().insert(0, cmStrCat(cmJoin(args, " "), ' '));
  }

  if (!compileCmds.empty()) {
    compileCmds.front().insert(0, cldeps);
  }

  const auto& extraCommands = this->GetMakefile()->GetSafeDefinition(
    cmStrCat("CMAKE_", lang, "_DEPENDS_EXTRA_COMMANDS"));
  if (!extraCommands.empty()) {
    auto commandList = cmExpandedList(extraCommands);
    compileCmds.insert(compileCmds.end(), commandList.cbegin(),
                       commandList.cend());
  }

  for (std::string& i : compileCmds) {
    i = cmStrCat(launcher, i);
    rulePlaceholderExpander->ExpandRuleVariables(this->GetLocalGenerator(), i,
                                                 vars);
  }

  rule.Command =
    this->GetLocalGenerator()->BuildCommandLine(compileCmds, config, config);

  // Write the rule for compiling file of the given language.
  rule.Comment = cmStrCat("Rule for compiling ", lang, " files.");
  rule.Description = cmStrCat("Building ", lang, " object $out");
  //this->GetGlobalGenerator()->AddRule(rule);

  // For Fastbuild
  this->WriteCompileFB(lang, config);
}

void cmFastbuildTargetGenerator::WriteCompileFB(const std::string& lang,const std::string& config)
{
  cmGlobalFastbuildGenerator* gfb = this->GetGlobalGenerator();
  cmMakefile* mf = this->GetMakefile();
  std::ostream& os = this->GetRulesFileStream();

  std::string executable = mf->GetSafeDefinition(cmStrCat("CMAKE_", lang, "_COMPILER"));
  std::string compiler_flags = mf->GetSafeDefinition(cmStrCat("CMAKE_", lang, "_FLAGS"));
  std::string flags_config = mf->GetSafeDefinition(cmStrCat("CMAKE_", lang, "_FLAGS_", cmSystemTools::UpperCase(config)));
  std::string create_static_library = mf->GetSafeDefinition("CMAKE_AR");

  std::string linker = mf->GetSafeDefinition("CMAKE_LINKER");
  std::string link_flags = "\"%1\" /OUT:\"%2\" ";
  link_flags += mf->GetSafeDefinition("LINK_OPTIONS");

  // if multiple CL.EXE write to the same .PDB file, please use /FS
  if (config == "Debug" || config == "RelWithDebInfo")
  {
    flags_config += " /FS";
  }

  gfb->WriteSectionHeader(os, "Compilers");
  gfb->WriteVariableFB(os, cmStrCat("Compiler", lang, config), "");
  gfb->WritePushScopeStruct(os);
  gfb->WriteVariableFB(os, "Compiler", gfb->Quote(executable));
  gfb->WriteVariableFB(os, "CompilerOptions", gfb->Quote(cmStrCat("/c %1 ", compiler_flags, " /Fo%2 ")));
  gfb->WriteVariableFB(os, "CompilerOptions", gfb->Quote(flags_config), "+");
  gfb->WriteVariableFB(os, "Librarian", gfb->Quote(create_static_library));
  gfb->WriteVariableFB(os, "LibrarianOptions", gfb->Quote(cmStrCat("/nologo ", link_flags)));
  gfb->WritePopScope(os);
  gfb->WriteVariableFB(os, "Linker", gfb->Quote(linker));
  gfb->WriteVariableFB(os, "LinkerOptions", gfb->Quote(link_flags));
}

void cmFastbuildTargetGenerator::WriteObjectBuildStatements(
  const std::string& config, const std::string& fileConfig,
  bool firstForConfig)
{
  // Write comments.
  cmGlobalFastbuildGenerator::WriteDivider(this->GetImplFileStream(fileConfig));
  this->GetImplFileStream(fileConfig)
    << "// NINJA Object build statements for "
    << cmState::GetTargetTypeName(this->GetGeneratorTarget()->GetType())
    << " target " << this->GetTargetName() << "\n\n";

  {
    std::vector<cmSourceFile const*> customCommands;
    this->GeneratorTarget->GetCustomCommands(customCommands, config);
    for (cmSourceFile const* sf : customCommands) {
      cmCustomCommand const* cc = sf->GetCustomCommand();
      this->GetLocalGenerator()->AddCustomCommandTarget(
        cc, this->GetGeneratorTarget());
      // Record the custom commands for this target. The container is used
      // in WriteObjectBuildStatement when called in a loop below.
      this->Configs[config].CustomCommands.push_back(cc);
    }
  }
  {
    std::vector<cmSourceFile const*> headerSources;
    this->GeneratorTarget->GetHeaderSources(headerSources, config);
    this->OSXBundleGenerator->GenerateMacOSXContentStatements(
      headerSources, this->Configs[fileConfig].MacOSXContentGenerator.get(),
      config);
  }
  {
    std::vector<cmSourceFile const*> extraSources;
    this->GeneratorTarget->GetExtraSources(extraSources, config);
    this->OSXBundleGenerator->GenerateMacOSXContentStatements(
      extraSources, this->Configs[fileConfig].MacOSXContentGenerator.get(),
      config);
  }
  if (firstForConfig) {
    cmProp pchExtension =
      this->GetMakefile()->GetDefinition("CMAKE_PCH_EXTENSION");

    std::vector<cmSourceFile const*> externalObjects;
    this->GeneratorTarget->GetExternalObjects(externalObjects, config);
    for (cmSourceFile const* sf : externalObjects) {
      auto objectFileName = this->GetGlobalGenerator()->ExpandCFGIntDir(
        this->GetSourceFilePath(sf), config);
      if (!cmSystemTools::StringEndsWith(objectFileName,
                                         cmToCStr(pchExtension))) {
        this->Configs[config].Objects.push_back(objectFileName);
      }
    }
  }

  {
    cmFastbuildBuild build("phony");
    build.Comment =
      cmStrCat("Order-only phony target for ", this->GetTargetName());
    build.Outputs.push_back(this->OrderDependsTargetForTarget(config));

    cmFastbuildDeps& orderOnlyDeps = build.OrderOnlyDeps;
    this->GetLocalGenerator()->AppendTargetDepends(
      this->GeneratorTarget, orderOnlyDeps, config, fileConfig,
      DependOnTargetOrderingFB);

    // Add order-only dependencies on other files associated with the target.
    cm::append(orderOnlyDeps, this->Configs[config].ExtraFiles);

    // Add order-only dependencies on custom command outputs.
    for (cmCustomCommand const* cc : this->Configs[config].CustomCommands) {
      cmCustomCommandGenerator ccg(*cc, config, this->GetLocalGenerator());
      const std::vector<std::string>& ccoutputs = ccg.GetOutputs();
      const std::vector<std::string>& ccbyproducts = ccg.GetByproducts();
      std::transform(ccoutputs.begin(), ccoutputs.end(),
                     std::back_inserter(orderOnlyDeps),
                     this->MapToFastbuildPath());
      std::transform(ccbyproducts.begin(), ccbyproducts.end(),
                     std::back_inserter(orderOnlyDeps),
                     this->MapToFastbuildPath());
    }

    std::sort(orderOnlyDeps.begin(), orderOnlyDeps.end());
    orderOnlyDeps.erase(
      std::unique(orderOnlyDeps.begin(), orderOnlyDeps.end()),
      orderOnlyDeps.end());

    // The phony target must depend on at least one input or ninja will explain
    // that "output ... of phony edge with no inputs doesn't exist" and
    // consider the phony output "dirty".
    if (orderOnlyDeps.empty()) {
      // Any path that always exists will work here.  It would be nice to
      // use just "." but that is not supported by Ninja < 1.7.
      std::string tgtDir = cmStrCat(
        this->LocalGenerator->GetCurrentBinaryDirectory(), '/',
        this->LocalGenerator->GetTargetDirectory(this->GeneratorTarget));
      orderOnlyDeps.push_back(this->ConvertToFastbuildPath(tgtDir));
    }

    this->GetGlobalGenerator()->WriteBuild(this->GetImplFileStream(fileConfig),
                                           build);
  }

  {
    std::vector<cmSourceFile const*> objectSources;
    this->GeneratorTarget->GetObjectSources(objectSources, config);

    for (cmSourceFile const* sf : objectSources) {
      this->WriteObjectBuildStatement(sf, config, fileConfig, firstForConfig);
    }
  }

  // For Fastbuild
  this->WriteObjectListFB(config);

  for (auto const& langDDIFiles : this->Configs[config].DDIFiles) {
    std::string const& language = langDDIFiles.first;
    cmFastbuildDeps const& ddiFiles = langDDIFiles.second;

    cmFastbuildBuild build(this->LanguageDyndepRule(language, config));
    build.Outputs.push_back(this->GetDyndepFilePath(language, config));
    build.ExplicitDeps = ddiFiles;

    this->WriteTargetDependInfo(language, config);

    // Make sure dyndep files for all our dependencies have already
    // been generated so that the '<LANG>Modules.json' files they
    // produced as side-effects are available for us to read.
    // Ideally we should depend on the '<LANG>Modules.json' files
    // from our dependencies directly, but we don't know which of
    // our dependencies produces them.  Fixing this will require
    // refactoring the Ninja generator to generate targets in
    // dependency order so that we can collect the needed information.
    this->GetLocalGenerator()->AppendTargetDepends(
      this->GeneratorTarget, build.OrderOnlyDeps, config, fileConfig,
      DependOnTargetArtifactFB);

    this->GetGlobalGenerator()->WriteBuild(this->GetImplFileStream(fileConfig),
                                           build);
  }

  this->GetImplFileStream(fileConfig) << "\n";

  if (!this->Configs[config].SwiftOutputMap.empty()) {
    std::string const mapFilePath =
      cmStrCat(this->GeneratorTarget->GetSupportDirectory(), '/', config, '/',
               "output-file-map.json");
    std::string const targetSwiftDepsPath = [this, config]() -> std::string {
      cmGeneratorTarget const* target = this->GeneratorTarget;
      if (cmProp name = target->GetProperty("Swift_DEPENDENCIES_FILE")) {
        return *name;
      }
      return this->ConvertToFastbuildPath(
        cmStrCat(target->GetSupportDirectory(), '/', config, '/',
                 target->GetName(), ".swiftdeps"));
    }();

    // build the global target dependencies
    // https://github.com/apple/swift/blob/master/docs/Driver.md#output-file-maps
    Json::Value deps(Json::objectValue);
    deps["swift-dependencies"] = targetSwiftDepsPath;
    this->Configs[config].SwiftOutputMap[""] = deps;

    cmGeneratedFileStream output(mapFilePath);
    output << this->Configs[config].SwiftOutputMap;
  }
}

void cmFastbuildTargetGenerator::WriteObjectListFB(const std::string& config)
{
  cmGlobalFastbuildGenerator* gfb = this->GetGlobalGenerator();

  std::vector<cmSourceFile const*> objectSources;
  this->GeneratorTarget->GetObjectSources(objectSources, config);
  std::vector<std::string> objectList;

  for (cmSourceFile const* sf : objectSources)
  {
    objectList.push_back(cmStrCat("\"", sf->GetFullPath(), "\""));
  }

  std::ostream& os = this->GetCommonFileStream();

  std::string language = this->GetGeneratorTarget()->GetLinkerLanguage(config);
  std::string project_name = this->GetTargetName();
  std::string current_source_dir = this->GetMakefile()->GetCurrentSourceDirectory();
  std::string target_output = this->GetTargetOutputDir(config);
  
  if (!gfb->IsMultiConfig())
  {
    gfb->WriteSectionHeader(os, project_name);
    gfb->WriteCommand(os, "ObjectList", gfb->Quote(cmStrCat(project_name, "-ObjectList")));
    gfb->WritePushScope(os);
    gfb->WriteCommand(os, "Using", cmStrCat(".Compiler", language, config));
    gfb->WriteArray(os, "CompilerInputFiles", objectList);
    gfb->WriteVariableFB(os, "CompilerOutputPath", gfb->Quote(target_output));
    gfb->WritePopScope(os);
  }
  else
  {
    gfb->WriteSectionHeader(os, cmStrCat(project_name, " : ", config));
    gfb->WriteCommand(os, "ObjectList", gfb->Quote(cmStrCat(project_name, "-ObjectList-", config, "")));
    gfb->WritePushScope(os);
    gfb->WriteCommand(os, "Using", cmStrCat(".Compiler", language, config));
    gfb->WriteArray(os, "CompilerInputFiles", objectList);
    gfb->WriteVariableFB(os, "CompilerOutputPath", gfb->Quote(target_output));
    gfb->WritePopScope(os);
  }
}

namespace {
cmFastbuildBuild GetScanBuildStatement(const std::string& ruleName,
                                   const std::string& ppFileName,
                                   bool compilePP, bool compilePPWithDefines,
                                   cmFastbuildBuild& objBuild, cmFastbuildVars& vars,
                                   std::string const& modmapFormat,
                                   const std::string& objectFileName,
                                   cmLocalGenerator* lg)
{
  cmFastbuildBuild scanBuild(ruleName);

  if (!ppFileName.empty()) {
    scanBuild.RspFile = cmStrCat(ppFileName, ".rsp");
  } else {
    scanBuild.RspFile = "$out.rsp";
  }

  if (compilePP) {
    // Move compilation dependencies to the scan/preprocessing build statement.
    std::swap(scanBuild.ExplicitDeps, objBuild.ExplicitDeps);
    std::swap(scanBuild.ImplicitDeps, objBuild.ImplicitDeps);
    std::swap(scanBuild.OrderOnlyDeps, objBuild.OrderOnlyDeps);
    std::swap(scanBuild.Variables["IN_ABS"], vars["IN_ABS"]);

    // The actual compilation will now use the preprocessed source.
    objBuild.ExplicitDeps.push_back(ppFileName);
  } else {
    // Copy compilation dependencies to the scan/preprocessing build statement.
    scanBuild.ExplicitDeps = objBuild.ExplicitDeps;
    scanBuild.ImplicitDeps = objBuild.ImplicitDeps;
    scanBuild.OrderOnlyDeps = objBuild.OrderOnlyDeps;
    scanBuild.Variables["IN_ABS"] = vars["IN_ABS"];
  }

  // Scanning and compilation generally use the same flags.
  scanBuild.Variables["FLAGS"] = vars["FLAGS"];

  if (compilePP && !compilePPWithDefines) {
    // Move preprocessor definitions to the scan/preprocessor build statement.
    std::swap(scanBuild.Variables["DEFINES"], vars["DEFINES"]);
  } else {
    // Copy preprocessor definitions to the scan/preprocessor build statement.
    scanBuild.Variables["DEFINES"] = vars["DEFINES"];
  }

  // Copy include directories to the preprocessor build statement.  The
  // Fortran compilation build statement still needs them for the INCLUDE
  // directive.
  scanBuild.Variables["INCLUDES"] = vars["INCLUDES"];

  // Tell dependency scanner the object file that will result from
  // compiling the source.
  scanBuild.Variables["OBJ_FILE"] = objectFileName;

  // Tell dependency scanner where to store dyndep intermediate results.
  std::string const& ddiFile = cmStrCat(objectFileName, ".ddi");
  scanBuild.Variables["DYNDEP_INTERMEDIATE_FILE"] = ddiFile;

  // Outputs of the scan/preprocessor build statement.
  if (!ppFileName.empty()) {
    scanBuild.Outputs.push_back(ppFileName);
    scanBuild.ImplicitOuts.push_back(ddiFile);
  } else {
    scanBuild.Outputs.push_back(ddiFile);
  }

  // Scanning always uses a depfile for preprocessor dependencies.
  std::string const& depFileName = cmStrCat(scanBuild.Outputs.front(), ".d");
  scanBuild.Variables["DEP_FILE"] =
    lg->ConvertToOutputFormat(depFileName, cmOutputConverter::SHELL);
  if (compilePP) {
    // The actual compilation does not need a depfile because it
    // depends on the already-preprocessed source.
    vars.erase("DEP_FILE");
  }

  if (!modmapFormat.empty()) {
    // XXX(modmap): If changing this path construction, change
    // `cmGlobalNinjaGenerator::WriteDyndep` to expect the corresponding
    // file path.
    std::string const ddModmapFile = cmStrCat(objectFileName, ".modmap");
    scanBuild.Variables["DYNDEP_MODULE_MAP_FILE"] = ddModmapFile;
    scanBuild.ImplicitOuts.push_back(ddModmapFile);
  }

  return scanBuild;
}
}

void cmFastbuildTargetGenerator::WriteObjectBuildStatement(
  cmSourceFile const* source, const std::string& config,
  const std::string& fileConfig, bool firstForConfig)
{
  std::string const language = source->GetLanguage();
  std::string const sourceFileName =
    language == "RC" ? source->GetFullPath() : this->GetSourceFilePath(source);
  std::string const objectDir = this->ConvertToFastbuildPath(
    cmStrCat(this->GeneratorTarget->GetSupportDirectory(),
             this->GetGlobalGenerator()->ConfigDirectory(config)));
  std::string const objectFileName =
    this->ConvertToFastbuildPath(this->GetObjectFilePath(source, config));
  std::string const objectFileDir =
    cmSystemTools::GetFilenamePath(objectFileName);

  std::string cmakeVarLang = cmStrCat("CMAKE_", language);

  // build response file name
  std::string cmakeLinkVar = cmStrCat(cmakeVarLang, "_RESPONSE_FILE_FLAG");

  cmProp flag = this->GetMakefile()->GetDefinition(cmakeLinkVar);

  bool const lang_supports_response =
    !(language == "RC" || (language == "CUDA" && !flag));
  int const commandLineLengthLimit =
    ((lang_supports_response && this->ForceResponseFile())) ? -1 : 0;

  cmFastbuildBuild objBuild(this->LanguageCompilerRule(language, config));
  cmFastbuildVars& vars = objBuild.Variables;
  vars["FLAGS"] = this->ComputeFlagsForObject(source, language, config);
  vars["DEFINES"] = this->ComputeDefines(source, language, config);
  vars["INCLUDES"] = this->ComputeIncludes(source, language, config);

  if (this->GetMakefile()->GetSafeDefinition(
        cmStrCat("CMAKE_", language, "_DEPFILE_FORMAT")) != "msvc"_s) {
    bool replaceExt(false);
    if (!language.empty()) {
      std::string repVar =
        cmStrCat("CMAKE_", language, "_DEPFILE_EXTENSION_REPLACE");
      replaceExt = this->Makefile->IsOn(repVar);
    }
    if (!replaceExt) {
      // use original code
      vars["DEP_FILE"] = this->GetLocalGenerator()->ConvertToOutputFormat(
        cmStrCat(objectFileName, ".d"), cmOutputConverter::SHELL);
    } else {
      // Replace the original source file extension with the
      // depend file extension.
      std::string dependFileName = cmStrCat(
        cmSystemTools::GetFilenameWithoutLastExtension(objectFileName), ".d");
      vars["DEP_FILE"] = this->GetLocalGenerator()->ConvertToOutputFormat(
        cmStrCat(objectFileDir, '/', dependFileName),
        cmOutputConverter::SHELL);
    }
  }

  this->ExportObjectCompileCommand(
    language, sourceFileName, objectDir, objectFileName, objectFileDir,
    vars["FLAGS"], vars["DEFINES"], vars["INCLUDES"], config);

  objBuild.Outputs.push_back(objectFileName);
  if (firstForConfig) {
    cmProp pchExtension =
      this->GetMakefile()->GetDefinition("CMAKE_PCH_EXTENSION");
    if (!cmSystemTools::StringEndsWith(objectFileName,
                                       cmToCStr(pchExtension))) {
      // Add this object to the list of object files.
      this->Configs[config].Objects.push_back(objectFileName);
    }
  }

  objBuild.ExplicitDeps.push_back(sourceFileName);

  // Add precompile headers dependencies
  std::vector<std::string> depList;

  std::vector<std::string> architectures;
  this->GeneratorTarget->GetAppleArchs(config, architectures);
  if (architectures.empty()) {
    architectures.emplace_back();
  }

  std::unordered_set<std::string> pchSources;
  for (const std::string& arch : architectures) {
    const std::string pchSource =
      this->GeneratorTarget->GetPchSource(config, language, arch);

    if (!pchSource.empty()) {
      pchSources.insert(pchSource);
    }
  }

  if (!pchSources.empty() && !source->GetProperty("SKIP_PRECOMPILE_HEADERS")) {
    for (const std::string& arch : architectures) {
      depList.push_back(
        this->GeneratorTarget->GetPchHeader(config, language, arch));
      if (pchSources.find(source->GetFullPath()) == pchSources.end()) {
        depList.push_back(
          this->GeneratorTarget->GetPchFile(config, language, arch));
      }
    }
  }

  if (cmProp objectDeps = source->GetProperty("OBJECT_DEPENDS")) {
    std::vector<std::string> objDepList = cmExpandedList(*objectDeps);
    std::copy(objDepList.begin(), objDepList.end(),
              std::back_inserter(depList));
  }

  if (!depList.empty()) {
    for (std::string& odi : depList) {
      if (cmSystemTools::FileIsFullPath(odi)) {
        odi = cmSystemTools::CollapseFullPath(odi);
      }
    }
    std::transform(depList.begin(), depList.end(),
                   std::back_inserter(objBuild.ImplicitDeps),
                   this->MapToFastbuildPath());
  }

  objBuild.OrderOnlyDeps.push_back(this->OrderDependsTargetForTarget(config));

  // If the source file is GENERATED and does not have a custom command
  // (either attached to this source file or another one), assume that one of
  // the target dependencies, OBJECT_DEPENDS or header file custom commands
  // will rebuild the file.
  if (source->GetIsGenerated() &&
      !source->GetPropertyAsBool("__CMAKE_GENERATED_BY_CMAKE") &&
      !source->GetCustomCommand() &&
      !this->GetGlobalGenerator()->HasCustomCommandOutput(sourceFileName)) {
    this->GetGlobalGenerator()->AddAssumedSourceDependencies(
      sourceFileName, objBuild.OrderOnlyDeps);
  }

  // For some cases we scan to dynamically discover dependencies.
  bool const needDyndep = this->NeedDyndep(language, config);
  bool const compilationPreprocesses =
    !this->NeedExplicitPreprocessing(language);

  std::string modmapFormat;
  if (needDyndep) {
    std::string const modmapFormatVar =
      cmStrCat("CMAKE_EXPERIMENTAL_", language, "_MODULE_MAP_FORMAT");
    modmapFormat = this->Makefile->GetSafeDefinition(modmapFormatVar);
  }

  if (needDyndep) {
    // If source/target has preprocessing turned off, we still need to
    // generate an explicit dependency step
    const auto srcpp = source->GetSafeProperty("Fortran_PREPROCESS");
    cmOutputConverter::FortranPreprocess preprocess =
      cmOutputConverter::GetFortranPreprocess(srcpp);
    if (preprocess == cmOutputConverter::FortranPreprocess::Unset) {
      const auto& tgtpp =
        this->GeneratorTarget->GetSafeProperty("Fortran_PREPROCESS");
      preprocess = cmOutputConverter::GetFortranPreprocess(tgtpp);
    }

    bool const compilePP = !compilationPreprocesses &&
      (preprocess != cmOutputConverter::FortranPreprocess::NotNeeded);
    bool const compilePPWithDefines =
      compilePP && this->CompileWithDefines(language);

    std::string scanRuleName;
    std::string ppFileName;
    if (compilePP) {
      scanRuleName = this->LanguagePreprocessAndScanRule(language, config);
      ppFileName = this->ConvertToFastbuildPath(
        this->GetPreprocessedFilePath(source, config));
    } else {
      scanRuleName = this->LanguageScanRule(language, config);
    }

    cmFastbuildBuild ppBuild = GetScanBuildStatement(
      scanRuleName, ppFileName, compilePP, compilePPWithDefines, objBuild,
      vars, modmapFormat, objectFileName, this->LocalGenerator);

    if (compilePP) {
      // In case compilation requires flags that are incompatible with
      // preprocessing, include them here.
      std::string const& postFlag = this->Makefile->GetSafeDefinition(
        cmStrCat("CMAKE_", language, "_POSTPROCESS_FLAG"));
      this->LocalGenerator->AppendFlags(vars["FLAGS"], postFlag);

      // Prepend source file's original directory as an include directory
      // so e.g. Fortran INCLUDE statements can look for files in it.
      std::vector<std::string> sourceDirectory;
      sourceDirectory.push_back(
        cmSystemTools::GetParentDirectory(source->GetFullPath()));

      std::string sourceDirectoryFlag = this->LocalGenerator->GetIncludeFlags(
        sourceDirectory, this->GeneratorTarget, language, false, false,
        config);

      vars["INCLUDES"] = cmStrCat(sourceDirectoryFlag, ' ', vars["INCLUDES"]);
    }

    if (firstForConfig) {
      std::string const ddiFile = cmStrCat(objectFileName, ".ddi");
      this->Configs[config].DDIFiles[language].push_back(ddiFile);
    }

    this->addPoolFastbuildVariable("JOB_POOL_COMPILE", this->GetGeneratorTarget(),
                               ppBuild.Variables);

    this->GetGlobalGenerator()->WriteBuild(this->GetImplFileStream(fileConfig),
                                           ppBuild, commandLineLengthLimit);

    std::string const dyndep = this->GetDyndepFilePath(language, config);
    objBuild.OrderOnlyDeps.push_back(dyndep);
    vars["dyndep"] = dyndep;

    if (!modmapFormat.empty()) {
      std::string const ddModmapFile = cmStrCat(objectFileName, ".modmap");
      vars["DYNDEP_MODULE_MAP_FILE"] = ddModmapFile;
      objBuild.OrderOnlyDeps.push_back(ddModmapFile);
    }
  }

  this->EnsureParentDirectoryExists(objectFileName);

  vars["OBJECT_DIR"] = this->GetLocalGenerator()->ConvertToOutputFormat(
    objectDir, cmOutputConverter::SHELL);
  vars["OBJECT_FILE_DIR"] = this->GetLocalGenerator()->ConvertToOutputFormat(
    objectFileDir, cmOutputConverter::SHELL);

  this->addPoolFastbuildVariable("JOB_POOL_COMPILE", this->GetGeneratorTarget(),
                             vars);

  if (!pchSources.empty() && !source->GetProperty("SKIP_PRECOMPILE_HEADERS")) {
    auto pchIt = pchSources.find(source->GetFullPath());
    if (pchIt != pchSources.end()) {
      this->addPoolFastbuildVariable("JOB_POOL_PRECOMPILE_HEADER",
                                 this->GetGeneratorTarget(), vars);
    }
  }

  this->SetMsvcTargetPdbVariable(vars, config);

  objBuild.RspFile = cmStrCat(objectFileName, ".rsp");

  if (language == "ISPC") {
    std::string const& objectName =
      this->GeneratorTarget->GetObjectName(source);
    std::string ispcSource =
      cmSystemTools::GetFilenameWithoutLastExtension(objectName);
    ispcSource = cmSystemTools::GetFilenameWithoutLastExtension(ispcSource);

    cmProp ispcSuffixProp =
      this->GeneratorTarget->GetProperty("ISPC_HEADER_SUFFIX");
    assert(ispcSuffixProp != nullptr);

    std::string ispcHeaderDirectory =
      this->GeneratorTarget->GetObjectDirectory(config);
    if (cmProp prop =
          this->GeneratorTarget->GetProperty("ISPC_HEADER_DIRECTORY")) {
      ispcHeaderDirectory =
        cmStrCat(this->LocalGenerator->GetBinaryDirectory(), '/', *prop);
    }

    std::string ispcHeader =
      cmStrCat(ispcHeaderDirectory, '/', ispcSource, *ispcSuffixProp);
    ispcHeader = this->ConvertToFastbuildPath(ispcHeader);

    // Make sure ninja knows what command generates the header
    objBuild.ImplicitOuts.push_back(ispcHeader);

    // Make sure ninja knows how to clean the generated header
    this->GetGlobalGenerator()->AddAdditionalCleanFile(ispcHeader, config);

    auto ispcSuffixes =
      detail::ComputeISPCObjectSuffixes(this->GeneratorTarget);
    if (ispcSuffixes.size() > 1) {
      std::string rootObjectDir =
        this->GeneratorTarget->GetObjectDirectory(config);
      auto ispcSideEfffectObjects = detail::ComputeISPCExtraObjects(
        objectName, rootObjectDir, ispcSuffixes);

      for (auto sideEffect : ispcSideEfffectObjects) {
        sideEffect = this->ConvertToFastbuildPath(sideEffect);
        objBuild.ImplicitOuts.emplace_back(sideEffect);
        this->GetGlobalGenerator()->AddAdditionalCleanFile(sideEffect, config);
      }
    }

    vars["ISPC_HEADER_FILE"] =
      this->GetLocalGenerator()->ConvertToOutputFormat(
        ispcHeader, cmOutputConverter::SHELL);
  } else {
    auto headers = this->GeneratorTarget->GetGeneratedISPCHeaders(config);
    if (!headers.empty()) {
      std::transform(headers.begin(), headers.end(), headers.begin(),
                     this->MapToFastbuildPath());
      objBuild.OrderOnlyDeps.insert(objBuild.OrderOnlyDeps.end(),
                                    headers.begin(), headers.end());
    }
  }

  if (language == "Swift") {
    this->EmitSwiftDependencyInfo(source, config);
  } else {
    this->GetGlobalGenerator()->WriteBuild(this->GetImplFileStream(fileConfig),
                                           objBuild, commandLineLengthLimit);
  }

  if (cmProp objectOutputs = source->GetProperty("OBJECT_OUTPUTS")) {
    std::string evaluatedObjectOutputs = cmGeneratorExpression::Evaluate(
      *objectOutputs, this->LocalGenerator, config);

    if (!evaluatedObjectOutputs.empty()) {
      cmFastbuildBuild build("phony");
      build.Comment = "Additional output files.";
      build.Outputs = cmExpandedList(evaluatedObjectOutputs);
      std::transform(build.Outputs.begin(), build.Outputs.end(),
                     build.Outputs.begin(), this->MapToFastbuildPath());
      build.ExplicitDeps = objBuild.Outputs;
      this->GetGlobalGenerator()->WriteBuild(
        this->GetImplFileStream(fileConfig), build);
    }
  }
}

void cmFastbuildTargetGenerator::WriteTargetDependInfo(std::string const& lang,
                                                   const std::string& config)
{
  Json::Value tdi(Json::objectValue);
  tdi["language"] = lang;
  tdi["compiler-id"] = this->Makefile->GetSafeDefinition(
    cmStrCat("CMAKE_", lang, "_COMPILER_ID"));

  std::string mod_dir;
  if (lang == "Fortran") {
    mod_dir = this->GeneratorTarget->GetFortranModuleDirectory(
      this->Makefile->GetHomeOutputDirectory());
  } else if (lang == "CXX") {
    mod_dir =
      cmSystemTools::CollapseFullPath(this->GeneratorTarget->ObjectDirectory);
  }
  if (mod_dir.empty()) {
    mod_dir = this->Makefile->GetCurrentBinaryDirectory();
  }
  tdi["module-dir"] = mod_dir;

  if (lang == "Fortran") {
    tdi["submodule-sep"] =
      this->Makefile->GetSafeDefinition("CMAKE_Fortran_SUBMODULE_SEP");
    tdi["submodule-ext"] =
      this->Makefile->GetSafeDefinition("CMAKE_Fortran_SUBMODULE_EXT");
  } else if (lang == "CXX") {
    // No extra information necessary.
  }

  tdi["dir-cur-bld"] = this->Makefile->GetCurrentBinaryDirectory();
  tdi["dir-cur-src"] = this->Makefile->GetCurrentSourceDirectory();
  tdi["dir-top-bld"] = this->Makefile->GetHomeOutputDirectory();
  tdi["dir-top-src"] = this->Makefile->GetHomeDirectory();

  Json::Value& tdi_include_dirs = tdi["include-dirs"] = Json::arrayValue;
  std::vector<std::string> includes;
  this->LocalGenerator->GetIncludeDirectories(includes, this->GeneratorTarget,
                                              lang, config);
  for (std::string const& i : includes) {
    // Convert the include directories the same way we do for -I flags.
    // See upstream ninja issue 1251.
    tdi_include_dirs.append(this->ConvertToFastbuildPath(i));
  }

  Json::Value& tdi_linked_target_dirs = tdi["linked-target-dirs"] =
    Json::arrayValue;
  for (std::string const& l : this->GetLinkedTargetDirectories(config)) {
    tdi_linked_target_dirs.append(l);
  }

  std::string const tdin = this->GetTargetDependInfoPath(lang, config);
  cmGeneratedFileStream tdif(tdin);
  tdif << tdi;
}

void cmFastbuildTargetGenerator::EmitSwiftDependencyInfo(
  cmSourceFile const* source, const std::string& config)
{
  std::string const sourceFilePath =
    this->ConvertToFastbuildPath(this->GetSourceFilePath(source));
  std::string const objectFilePath =
    this->ConvertToFastbuildPath(this->GetObjectFilePath(source, config));
  std::string const swiftDepsPath = [source, objectFilePath]() -> std::string {
    if (cmProp name = source->GetProperty("Swift_DEPENDENCIES_FILE")) {
      return *name;
    }
    return cmStrCat(objectFilePath, ".swiftdeps");
  }();
  std::string const swiftDiaPath = [source, objectFilePath]() -> std::string {
    if (cmProp name = source->GetProperty("Swift_DIAGNOSTICS_FILE")) {
      return *name;
    }
    return cmStrCat(objectFilePath, ".dia");
  }();
  std::string const makeDepsPath = [this, source, config]() -> std::string {
    cmLocalFastbuildGenerator const* local = this->GetLocalGenerator();
    std::string const objectFileName =
      this->ConvertToFastbuildPath(this->GetObjectFilePath(source, config));
    std::string const objectFileDir =
      cmSystemTools::GetFilenamePath(objectFileName);

    if (this->Makefile->IsOn("CMAKE_Swift_DEPFLE_EXTNSION_REPLACE")) {
      std::string dependFileName = cmStrCat(
        cmSystemTools::GetFilenameWithoutLastExtension(objectFileName), ".d");
      return local->ConvertToOutputFormat(
        cmStrCat(objectFileDir, '/', dependFileName),
        cmOutputConverter::SHELL);
    }
    return local->ConvertToOutputFormat(cmStrCat(objectFileName, ".d"),
                                        cmOutputConverter::SHELL);
  }();

  // build the source file mapping
  // https://github.com/apple/swift/blob/master/docs/Driver.md#output-file-maps
  Json::Value entry = Json::Value(Json::objectValue);
  entry["object"] = objectFilePath;
  entry["dependencies"] = makeDepsPath;
  entry["swift-dependencies"] = swiftDepsPath;
  entry["diagnostics"] = swiftDiaPath;
  this->Configs[config].SwiftOutputMap[sourceFilePath] = entry;
}

void cmFastbuildTargetGenerator::ExportObjectCompileCommand(
  std::string const& language, std::string const& sourceFileName,
  std::string const& objectDir, std::string const& objectFileName,
  std::string const& objectFileDir, std::string const& flags,
  std::string const& defines, std::string const& includes,
  std::string const& outputConfig)
{
  if (!this->GeneratorTarget->GetPropertyAsBool("EXPORT_COMPILE_COMMANDS")) {
    return;
  }

  cmRulePlaceholderExpander::RuleVariables compileObjectVars;
  compileObjectVars.Language = language.c_str();

  std::string escapedSourceFileName = sourceFileName;

  if (!cmSystemTools::FileIsFullPath(sourceFileName)) {
    escapedSourceFileName =
      cmSystemTools::CollapseFullPath(escapedSourceFileName,
                                      this->GetGlobalGenerator()
                                        ->GetCMakeInstance()
                                        ->GetHomeOutputDirectory());
  }

  escapedSourceFileName = this->LocalGenerator->ConvertToOutputFormat(
    escapedSourceFileName, cmOutputConverter::SHELL);

  compileObjectVars.Source = escapedSourceFileName.c_str();
  compileObjectVars.Object = objectFileName.c_str();
  compileObjectVars.ObjectDir = objectDir.c_str();
  compileObjectVars.ObjectFileDir = objectFileDir.c_str();
  compileObjectVars.Flags = flags.c_str();
  compileObjectVars.Defines = defines.c_str();
  compileObjectVars.Includes = includes.c_str();

  // Rule for compiling object file.
  std::vector<std::string> compileCmds;
  if (language == "CUDA") {
    std::string cmdVar;
    if (this->GeneratorTarget->GetPropertyAsBool(
          "CUDA_SEPARABLE_COMPILATION")) {
      cmdVar = "CMAKE_CUDA_COMPILE_SEPARABLE_COMPILATION";
    } else if (this->GeneratorTarget->GetPropertyAsBool(
                 "CUDA_PTX_COMPILATION")) {
      cmdVar = "CMAKE_CUDA_COMPILE_PTX_COMPILATION";
    } else {
      cmdVar = "CMAKE_CUDA_COMPILE_WHOLE_COMPILATION";
    }
    const std::string& compileCmd =
      this->GetMakefile()->GetRequiredDefinition(cmdVar);
    cmExpandList(compileCmd, compileCmds);
  } else {
    const std::string cmdVar = cmStrCat("CMAKE_", language, "_COMPILE_OBJECT");
    const std::string& compileCmd =
      this->GetMakefile()->GetRequiredDefinition(cmdVar);
    cmExpandList(compileCmd, compileCmds);
  }

  std::unique_ptr<cmRulePlaceholderExpander> rulePlaceholderExpander(
    this->GetLocalGenerator()->CreateRulePlaceholderExpander());

  for (std::string& i : compileCmds) {
    // no launcher for CMAKE_EXPORT_COMPILE_COMMANDS
    rulePlaceholderExpander->ExpandRuleVariables(this->GetLocalGenerator(), i,
                                                 compileObjectVars);
  }

  std::string cmdLine = this->GetLocalGenerator()->BuildCommandLine(
    compileCmds, outputConfig, outputConfig);

  this->GetGlobalGenerator()->AddCXXCompileCommand(cmdLine, sourceFileName);
}

void cmFastbuildTargetGenerator::AdditionalCleanFiles(const std::string& config)
{
  if (cmProp prop_value =
        this->GeneratorTarget->GetProperty("ADDITIONAL_CLEAN_FILES")) {
    cmLocalFastbuildGenerator* lg = this->LocalGenerator;
    std::vector<std::string> cleanFiles;
    cmExpandList(cmGeneratorExpression::Evaluate(*prop_value, lg, config,
                                                 this->GeneratorTarget),
                 cleanFiles);
    std::string const& binaryDir = lg->GetCurrentBinaryDirectory();
    cmGlobalFastbuildGenerator* gg = lg->GetGlobalFastbuildGenerator();
    for (std::string const& cleanFile : cleanFiles) {
      // Support relative paths
      gg->AddAdditionalCleanFile(
        cmSystemTools::CollapseFullPath(cleanFile, binaryDir), config);
    }
  }
}

cmFastbuildDeps cmFastbuildTargetGenerator::GetObjects(const std::string& config) const
{
  auto const it = this->Configs.find(config);
  if (it != this->Configs.end()) {
    return it->second.Objects;
  }
  return {};
}

void cmFastbuildTargetGenerator::EnsureDirectoryExists(
  const std::string& path) const
{
  if (cmSystemTools::FileIsFullPath(path)) {
    cmSystemTools::MakeDirectory(path);
  } else {
    cmGlobalFastbuildGenerator* gg = this->GetGlobalGenerator();
    std::string fullPath = gg->GetCMakeInstance()->GetHomeOutputDirectory();
    // Also ensures their is a trailing slash.
    gg->StripFastbuildOutputPathPrefixAsSuffix(fullPath);
    fullPath += path;
    cmSystemTools::MakeDirectory(fullPath);
  }
}

void cmFastbuildTargetGenerator::EnsureParentDirectoryExists(
  const std::string& path) const
{
  this->EnsureDirectoryExists(cmSystemTools::GetParentDirectory(path));
}

void cmFastbuildTargetGenerator::MacOSXContentGeneratorType::operator()(
  cmSourceFile const& source, const char* pkgloc, const std::string& config)
{
  // Skip OS X content when not building a Framework or Bundle.
  if (!this->Generator->GetGeneratorTarget()->IsBundleOnApple()) {
    return;
  }

  std::string macdir =
    this->Generator->OSXBundleGenerator->InitMacOSXContentDirectory(pkgloc,
                                                                    config);

  // Reject files that collide with files from the Ninja file's native config.
  if (config != this->FileConfig) {
    std::string nativeMacdir =
      this->Generator->OSXBundleGenerator->InitMacOSXContentDirectory(
        pkgloc, this->FileConfig);
    if (macdir == nativeMacdir) {
      return;
    }
  }

  // Get the input file location.
  std::string input = source.GetFullPath();
  input = this->Generator->GetGlobalGenerator()->ConvertToFastbuildPath(input);

  // Get the output file location.
  std::string output =
    cmStrCat(macdir, '/', cmSystemTools::GetFilenameName(input));
  output = this->Generator->GetGlobalGenerator()->ConvertToFastbuildPath(output);

  // Write a build statement to copy the content into the bundle.
  this->Generator->GetGlobalGenerator()->WriteMacOSXContentBuild(
    input, output, this->FileConfig);

  // Add as a dependency to the target so that it gets called.
  this->Generator->Configs[config].ExtraFiles.push_back(std::move(output));
}

void cmFastbuildTargetGenerator::addPoolFastbuildVariable(
  const std::string& pool_property, cmGeneratorTarget* target,
  cmFastbuildVars& vars)
{
  cmProp pool = target->GetProperty(pool_property);
  if (pool) {
    vars["pool"] = *pool;
  }
}

bool cmFastbuildTargetGenerator::ForceResponseFile()
{
  static std::string const forceRspFile = "CMAKE_FASTBUILD_FORCE_RESPONSE_FILE";
  return (this->GetMakefile()->IsDefinitionSet(forceRspFile) ||
          cmSystemTools::HasEnv(forceRspFile));
}
