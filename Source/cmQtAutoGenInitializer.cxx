/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#include "cmQtAutoGenInitializer.h"

#include "cmQtAutoGen.h"
#include "cmQtAutoGenGlobalInitializer.h"

#include "cmAlgorithms.h"
#include "cmCustomCommand.h"
#include "cmCustomCommandLines.h"
#include "cmFilePathChecksum.h"
#include "cmGeneratorExpression.h"
#include "cmGeneratorTarget.h"
#include "cmGlobalGenerator.h"
#include "cmLinkItem.h"
#include "cmListFileCache.h"
#include "cmLocalGenerator.h"
#include "cmMakefile.h"
#include "cmMessageType.h"
#include "cmOutputConverter.h"
#include "cmPolicies.h"
#include "cmSourceFile.h"
#include "cmSourceFileLocationKind.h"
#include "cmSourceGroup.h"
#include "cmState.h"
#include "cmStateTypes.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmTarget.h"
#include "cmake.h"
#include "cmsys/SystemInformation.hxx"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <cm/memory>

namespace {

std::size_t GetParallelCPUCount()
{
  static std::size_t count = 0;
  // Detect only on the first call
  if (count == 0) {
    cmsys::SystemInformation info;
    info.RunCPUCheck();
    count = info.GetNumberOfPhysicalCPU();
    count = std::max<std::size_t>(count, 1);
    count = std::min<std::size_t>(count, cmQtAutoGen::ParallelMax);
  }
  return count;
}

std::string FileProjectRelativePath(cmMakefile* makefile,
                                    std::string const& fileName)
{
  std::string res;
  {
    std::string pSource = cmSystemTools::RelativePath(
      makefile->GetCurrentSourceDirectory(), fileName);
    std::string pBinary = cmSystemTools::RelativePath(
      makefile->GetCurrentBinaryDirectory(), fileName);
    if (pSource.size() < pBinary.size()) {
      res = std::move(pSource);
    } else if (pBinary.size() < fileName.size()) {
      res = std::move(pBinary);
    } else {
      res = fileName;
    }
  }
  return res;
}

/**
 * Tests if targetDepend is a STATIC_LIBRARY and if any of its
 * recursive STATIC_LIBRARY dependencies depends on targetOrigin
 * (STATIC_LIBRARY cycle).
 */
bool StaticLibraryCycle(cmGeneratorTarget const* targetOrigin,
                        cmGeneratorTarget const* targetDepend,
                        std::string const& config)
{
  bool cycle = false;
  if ((targetOrigin->GetType() == cmStateEnums::STATIC_LIBRARY) &&
      (targetDepend->GetType() == cmStateEnums::STATIC_LIBRARY)) {
    std::set<cmGeneratorTarget const*> knownLibs;
    std::deque<cmGeneratorTarget const*> testLibs;

    // Insert initial static_library dependency
    knownLibs.insert(targetDepend);
    testLibs.push_back(targetDepend);

    while (!testLibs.empty()) {
      cmGeneratorTarget const* testTarget = testLibs.front();
      testLibs.pop_front();
      // Check if the test target is the origin target (cycle)
      if (testTarget == targetOrigin) {
        cycle = true;
        break;
      }
      // Collect all static_library dependencies from the test target
      cmLinkImplementationLibraries const* libs =
        testTarget->GetLinkImplementationLibraries(config);
      if (libs != nullptr) {
        for (cmLinkItem const& item : libs->Libraries) {
          cmGeneratorTarget const* depTarget = item.Target;
          if ((depTarget != nullptr) &&
              (depTarget->GetType() == cmStateEnums::STATIC_LIBRARY) &&
              knownLibs.insert(depTarget).second) {
            testLibs.push_back(depTarget);
          }
        }
      }
    }
  }
  return cycle;
}

/** Sanitizes file search paths */
class SearchPathSanitizer
{
public:
  SearchPathSanitizer(cmMakefile* makefile)
    : SourcePath_(makefile->GetCurrentSourceDirectory())
  {
  }
  std::vector<std::string> operator()(
    std::vector<std::string> const& paths) const;

private:
  std::string SourcePath_;
};

std::vector<std::string> SearchPathSanitizer::operator()(
  std::vector<std::string> const& paths) const
{
  std::vector<std::string> res;
  res.reserve(paths.size());
  for (std::string const& srcPath : paths) {
    // Collapse relative paths
    std::string path = cmSystemTools::CollapseFullPath(srcPath, SourcePath_);
    // Remove suffix slashes
    while (cmHasSuffix(path, '/')) {
      path.pop_back();
    }
    // Accept only non empty paths
    if (!path.empty()) {
      res.emplace_back(std::move(path));
    }
  }
  return res;
}
} // End of unnamed namespace

cmQtAutoGenInitializer::InfoWriter::InfoWriter(std::string const& filename)
{
  Ofs_.SetCopyIfDifferent(true);
  Ofs_.Open(filename, false, true);
}

template <class IT>
std::string cmQtAutoGenInitializer::InfoWriter::ListJoin(IT it_begin,
                                                         IT it_end)
{
  std::string res;
  for (IT it = it_begin; it != it_end; ++it) {
    if (it != it_begin) {
      res += ';';
    }
    for (const char* c = it->c_str(); *c; ++c) {
      if (*c == '"') {
        // Escape the double quote to avoid ending the argument.
        res += "\\\"";
      } else if (*c == '$') {
        // Escape the dollar to avoid expanding variables.
        res += "\\$";
      } else if (*c == '\\') {
        // Escape the backslash to avoid other escapes.
        res += "\\\\";
      } else if (*c == ';') {
        // Escape the semicolon to avoid list expansion.
        res += "\\;";
      } else {
        // Other characters will be parsed correctly.
        res += *c;
      }
    }
  }
  return res;
}

inline std::string cmQtAutoGenInitializer::InfoWriter::ConfigKey(
  cm::string_view key, std::string const& config)
{
  return cmStrCat(key, "_", config);
}

void cmQtAutoGenInitializer::InfoWriter::Write(cm::string_view key,
                                               std::string const& value)
{
  Ofs_ << "set(" << key << " " << cmOutputConverter::EscapeForCMake(value)
       << ")\n";
};

void cmQtAutoGenInitializer::InfoWriter::WriteUInt(cm::string_view key,
                                                   unsigned int value)
{
  Ofs_ << "set(" << key << " " << value << ")\n";
};

template <class C>
void cmQtAutoGenInitializer::InfoWriter::WriteStrings(cm::string_view key,
                                                      C const& container)
{
  Ofs_ << "set(" << key << " \""
       << ListJoin(container.begin(), container.end()) << "\")\n";
}

void cmQtAutoGenInitializer::InfoWriter::WriteConfig(
  cm::string_view key, std::map<std::string, std::string> const& map)
{
  for (auto const& item : map) {
    Write(ConfigKey(key, item.first), item.second);
  }
};

template <class C>
void cmQtAutoGenInitializer::InfoWriter::WriteConfigStrings(
  cm::string_view key, std::map<std::string, C> const& map)
{
  for (auto const& item : map) {
    WriteStrings(ConfigKey(key, item.first), item.second);
  }
}

void cmQtAutoGenInitializer::InfoWriter::WriteNestedLists(
  cm::string_view key, std::vector<std::vector<std::string>> const& lists)
{
  std::vector<std::string> seplist;
  seplist.reserve(lists.size());
  for (std::vector<std::string> const& list : lists) {
    seplist.push_back(cmStrCat("{", ListJoin(list.begin(), list.end()), "}"));
  }
  Write(key, cmJoin(seplist, cmQtAutoGen::ListSep));
};

cmQtAutoGenInitializer::cmQtAutoGenInitializer(
  cmQtAutoGenGlobalInitializer* globalInitializer,
  cmGeneratorTarget* genTarget, IntegerVersion const& qtVersion,
  bool mocEnabled, bool uicEnabled, bool rccEnabled, bool globalAutogenTarget,
  bool globalAutoRccTarget)
  : GlobalInitializer(globalInitializer)
  , GenTarget(genTarget)
  , GlobalGen(genTarget->GetGlobalGenerator())
  , LocalGen(genTarget->GetLocalGenerator())
  , Makefile(genTarget->Makefile)
  , QtVersion(qtVersion)
{
  AutogenTarget.GlobalTarget = globalAutogenTarget;
  Moc.Enabled = mocEnabled;
  Uic.Enabled = uicEnabled;
  Rcc.Enabled = rccEnabled;
  Rcc.GlobalTarget = globalAutoRccTarget;
}

bool cmQtAutoGenInitializer::InitCustomTargets()
{
  // Configurations
  this->MultiConfig = this->GlobalGen->IsMultiConfig();
  this->ConfigDefault = this->Makefile->GetConfigurations(this->ConfigsList);
  if (this->ConfigsList.empty()) {
    this->ConfigsList.push_back(this->ConfigDefault);
  }

  // Verbosity
  this->Verbosity = this->Makefile->GetSafeDefinition("CMAKE_AUTOGEN_VERBOSE");
  if (!this->Verbosity.empty()) {
    unsigned long iVerb = 0;
    if (!cmStrToULong(this->Verbosity, &iVerb)) {
      // Non numeric verbosity
      this->Verbosity = cmIsOn(this->Verbosity) ? "1" : "0";
    }
  }

  // Targets FOLDER
  {
    const char* folder =
      this->Makefile->GetState()->GetGlobalProperty("AUTOMOC_TARGETS_FOLDER");
    if (folder == nullptr) {
      folder = this->Makefile->GetState()->GetGlobalProperty(
        "AUTOGEN_TARGETS_FOLDER");
    }
    // Inherit FOLDER property from target (#13688)
    if (folder == nullptr) {
      folder = this->GenTarget->GetProperty("FOLDER");
    }
    if (folder != nullptr) {
      this->TargetsFolder = folder;
    }
  }

  // Check status of policy CMP0071
  {
    cmPolicies::PolicyStatus const CMP0071_status =
      this->Makefile->GetPolicyStatus(cmPolicies::CMP0071);
    switch (CMP0071_status) {
      case cmPolicies::WARN:
        this->CMP0071Warn = true;
        CM_FALLTHROUGH;
      case cmPolicies::OLD:
        // Ignore GENERATED file
        break;
      case cmPolicies::REQUIRED_IF_USED:
      case cmPolicies::REQUIRED_ALWAYS:
      case cmPolicies::NEW:
        // Process GENERATED file
        this->CMP0071Accept = true;
        break;
    }
  }

  // Common directories
  {
    // Collapsed current binary directory
    std::string const cbd = cmSystemTools::CollapseFullPath(
      std::string(), this->Makefile->GetCurrentBinaryDirectory());

    // Info directory
    this->Dir.Info = cmStrCat(cbd, "/CMakeFiles/", this->GenTarget->GetName(),
                              "_autogen.dir");
    cmSystemTools::ConvertToUnixSlashes(this->Dir.Info);

    // Build directory
    this->Dir.Build = this->GenTarget->GetSafeProperty("AUTOGEN_BUILD_DIR");
    if (this->Dir.Build.empty()) {
      this->Dir.Build =
        cmStrCat(cbd, '/', this->GenTarget->GetName(), "_autogen");
    }
    cmSystemTools::ConvertToUnixSlashes(this->Dir.Build);
    // Cleanup build directory
    this->AddCleanFile(this->Dir.Build);

    // Working directory
    this->Dir.Work = cbd;
    cmSystemTools::ConvertToUnixSlashes(this->Dir.Work);

    // Include directory
    this->Dir.Include = cmStrCat(this->Dir.Build, "/include");
    if (this->MultiConfig) {
      this->Dir.Include += "_$<CONFIG>";
    }
    // Per config include directories
    if (this->MultiConfig) {
      for (std::string const& cfg : this->ConfigsList) {
        std::string& dir = this->Dir.ConfigInclude[cfg];
        dir = cmStrCat(this->Dir.Build, "/include_", cfg);
      }
    }
  }

  // Moc, Uic and _autogen target settings
  if (this->MocOrUicEnabled()) {
    // Init moc specific settings
    if (this->Moc.Enabled && !InitMoc()) {
      return false;
    }

    // Init uic specific settings
    if (this->Uic.Enabled && !InitUic()) {
      return false;
    }

    // Autogen target name
    this->AutogenTarget.Name =
      cmStrCat(this->GenTarget->GetName(), "_autogen");

    // Autogen target parallel processing
    this->AutogenTarget.Parallel =
      this->GenTarget->GetSafeProperty("AUTOGEN_PARALLEL");
    if (this->AutogenTarget.Parallel.empty() ||
        (this->AutogenTarget.Parallel == "AUTO")) {
      // Autodetect number of CPUs
      this->AutogenTarget.Parallel = std::to_string(GetParallelCPUCount());
    }

    // Autogen target info and settings files
    {
      this->AutogenTarget.InfoFile =
        cmStrCat(this->Dir.Info, "/AutogenInfo.cmake");

      this->AutogenTarget.SettingsFile =
        cmStrCat(this->Dir.Info, "/AutogenOldSettings.txt");

      if (this->MultiConfig) {
        for (std::string const& cfg : this->ConfigsList) {
          std::string& filename = this->AutogenTarget.ConfigSettingsFile[cfg];
          filename =
            AppendFilenameSuffix(this->AutogenTarget.SettingsFile, "_" + cfg);
          this->AddCleanFile(filename);
        }
      } else {
        this->AddCleanFile(this->AutogenTarget.SettingsFile);
      }

      this->AutogenTarget.ParseCacheFile =
        cmStrCat(this->Dir.Info, "/ParseCache.txt");
      this->AddCleanFile(this->AutogenTarget.ParseCacheFile);
    }

    // Autogen target: Compute user defined dependencies
    {
      this->AutogenTarget.DependOrigin =
        this->GenTarget->GetPropertyAsBool("AUTOGEN_ORIGIN_DEPENDS");

      std::string const deps =
        this->GenTarget->GetSafeProperty("AUTOGEN_TARGET_DEPENDS");
      if (!deps.empty()) {
        for (std::string const& depName : cmExpandedList(deps)) {
          // Allow target and file dependencies
          auto* depTarget = this->Makefile->FindTargetToUse(depName);
          if (depTarget != nullptr) {
            this->AutogenTarget.DependTargets.insert(depTarget);
          } else {
            this->AutogenTarget.DependFiles.insert(depName);
          }
        }
      }
    }

    // CMAKE_AUTOMOC_RELAXED_MODE deprecation warning
    if (this->Moc.Enabled) {
      if (this->Makefile->IsOn("CMAKE_AUTOMOC_RELAXED_MODE")) {
        this->Makefile->IssueMessage(
          MessageType::AUTHOR_WARNING,
          cmStrCat("AUTOMOC: CMAKE_AUTOMOC_RELAXED_MODE is "
                   "deprecated an will be removed in the future.  Consider "
                   "disabling it and converting the target ",
                   this->GenTarget->GetName(), " to regular mode."));
      }
    }
  }

  // Init rcc specific settings
  if (this->Rcc.Enabled && !InitRcc()) {
    return false;
  }

  // Add autogen include directory to the origin target INCLUDE_DIRECTORIES
  if (this->MocOrUicEnabled() || (this->Rcc.Enabled && this->MultiConfig)) {
    this->GenTarget->AddIncludeDirectory(this->Dir.Include, true);
  }

  // Scan files
  if (!this->InitScanFiles()) {
    return false;
  }

  // Create autogen target
  if (this->MocOrUicEnabled() && !this->InitAutogenTarget()) {
    return false;
  }

  // Create rcc targets
  if (this->Rcc.Enabled && !this->InitRccTargets()) {
    return false;
  }

  return true;
}

bool cmQtAutoGenInitializer::InitMoc()
{
  // Mocs compilation file
  this->Moc.MocsCompilation =
    cmStrCat(this->Dir.Build, "/mocs_compilation.cpp");

  // Moc predefs command
  if (this->GenTarget->GetPropertyAsBool("AUTOMOC_COMPILER_PREDEFINES") &&
      (this->QtVersion >= IntegerVersion(5, 8))) {
    this->Moc.PredefsCmd = this->Makefile->GetSafeDefinition(
      "CMAKE_CXX_COMPILER_PREDEFINES_COMMAND");
  }

  // Moc includes
  {
    SearchPathSanitizer sanitizer(this->Makefile);
    auto GetIncludeDirs =
      [this, &sanitizer](std::string const& cfg) -> std::vector<std::string> {
      // Get the include dirs for this target, without stripping the implicit
      // include dirs off, see issue #13667.
      std::vector<std::string> dirs;
      bool const appendImplicit = (this->QtVersion.Major >= 5);
      this->LocalGen->GetIncludeDirectoriesImplicit(
        dirs, this->GenTarget, "CXX", cfg, false, appendImplicit);
      return sanitizer(dirs);
    };

    // Default configuration include directories
    this->Moc.Includes = GetIncludeDirs(this->ConfigDefault);
    // Other configuration settings
    if (this->MultiConfig) {
      for (std::string const& cfg : this->ConfigsList) {
        std::vector<std::string> dirs = GetIncludeDirs(cfg);
        if (dirs != this->Moc.Includes) {
          this->Moc.ConfigIncludes[cfg] = std::move(dirs);
        }
      }
    }
  }

  // Moc compile definitions
  {
    auto GetCompileDefinitions =
      [this](std::string const& cfg) -> std::set<std::string> {
      std::set<std::string> defines;
      this->LocalGen->GetTargetDefines(this->GenTarget, cfg, "CXX", defines);
#ifdef _WIN32
      if (this->Moc.PredefsCmd.empty()) {
        // Add WIN32 definition if we don't have a moc_predefs.h
        defines.insert("WIN32");
      }
#endif
      return defines;
    };

    // Default configuration defines
    this->Moc.Defines = GetCompileDefinitions(this->ConfigDefault);
    // Other configuration defines
    if (this->MultiConfig) {
      for (std::string const& cfg : this->ConfigsList) {
        std::set<std::string> defines = GetCompileDefinitions(cfg);
        if (defines != this->Moc.Defines) {
          this->Moc.ConfigDefines[cfg] = std::move(defines);
        }
      }
    }
  }

  // Moc executable
  {
    if (!this->GetQtExecutable(this->Moc, "moc", false)) {
      return false;
    }
    // Let the _autogen target depend on the moc executable
    if (this->Moc.ExecutableTarget != nullptr) {
      this->AutogenTarget.DependTargets.insert(
        this->Moc.ExecutableTarget->Target);
    }
  }

  return true;
}

bool cmQtAutoGenInitializer::InitUic()
{
  // Uic search paths
  {
    std::string const usp =
      this->GenTarget->GetSafeProperty("AUTOUIC_SEARCH_PATHS");
    if (!usp.empty()) {
      this->Uic.SearchPaths =
        SearchPathSanitizer(this->Makefile)(cmExpandedList(usp));
    }
  }
  // Uic target options
  {
    auto UicGetOpts =
      [this](std::string const& cfg) -> std::vector<std::string> {
      std::vector<std::string> opts;
      this->GenTarget->GetAutoUicOptions(opts, cfg);
      return opts;
    };

    // Default settings
    this->Uic.Options = UicGetOpts(this->ConfigDefault);

    // Configuration specific settings
    if (this->MultiConfig) {
      for (std::string const& cfg : this->ConfigsList) {
        std::vector<std::string> options = UicGetOpts(cfg);
        if (options != this->Uic.Options) {
          this->Uic.ConfigOptions[cfg] = std::move(options);
        }
      }
    }
  }

  // Uic executable
  {
    if (!this->GetQtExecutable(this->Uic, "uic", true)) {
      return false;
    }
    // Let the _autogen target depend on the uic executable
    if (this->Uic.ExecutableTarget != nullptr) {
      this->AutogenTarget.DependTargets.insert(
        this->Uic.ExecutableTarget->Target);
    }
  }

  return true;
}

bool cmQtAutoGenInitializer::InitRcc()
{
  // Rcc executable
  {
    if (!this->GetQtExecutable(this->Rcc, "rcc", false)) {
      return false;
    }
    // Evaluate test output on demand
    CompilerFeatures& features = *this->Rcc.ExecutableFeatures;
    if (!features.Evaluated) {
      // Look for list options
      if (this->QtVersion.Major == 5 || this->QtVersion.Major == 6) {
        if (features.HelpOutput.find("--list") != std::string::npos) {
          features.ListOptions.emplace_back("--list");
        } else if (features.HelpOutput.find("-list") != std::string::npos) {
          features.ListOptions.emplace_back("-list");
        }
      }
      // Evaluation finished
      features.Evaluated = true;
    }
  }

  return true;
}

bool cmQtAutoGenInitializer::InitScanFiles()
{
  cmake const* cm = this->Makefile->GetCMakeInstance();
  auto const& kw = this->GlobalInitializer->kw();

  auto makeMUFile = [this, &kw](cmSourceFile* sf, std::string const& fullPath,
                                bool muIt) -> MUFileHandle {
    MUFileHandle muf = cm::make_unique<MUFile>();
    muf->FullPath = fullPath;
    muf->SF = sf;
    muf->Generated = sf->GetIsGenerated();
    bool const skipAutogen = sf->GetPropertyAsBool(kw.SKIP_AUTOGEN);
    muf->SkipMoc = this->Moc.Enabled &&
      (skipAutogen || sf->GetPropertyAsBool(kw.SKIP_AUTOMOC));
    muf->SkipUic = this->Uic.Enabled &&
      (skipAutogen || sf->GetPropertyAsBool(kw.SKIP_AUTOUIC));
    if (muIt) {
      muf->MocIt = this->Moc.Enabled && !muf->SkipMoc;
      muf->UicIt = this->Uic.Enabled && !muf->SkipUic;
    }
    return muf;
  };

  auto addMUFile = [&](MUFileHandle&& muf, bool isHeader) {
    if ((muf->MocIt || muf->UicIt) && muf->Generated) {
      this->AutogenTarget.FilesGenerated.emplace_back(muf.get());
    }
    if (isHeader) {
      this->AutogenTarget.Headers.emplace(muf->SF, std::move(muf));
    } else {
      this->AutogenTarget.Sources.emplace(muf->SF, std::move(muf));
    }
  };

  // Scan through target files
  {
    // Scan through target files
    std::vector<cmSourceFile*> srcFiles;
    this->GenTarget->GetConfigCommonSourceFiles(srcFiles);
    for (cmSourceFile* sf : srcFiles) {
      // sf->GetExtension() is only valid after sf->ResolveFullPath() ...
      // Since we're iterating over source files that might be not in the
      // target we need to check for path errors (not existing files).
      std::string pathError;
      std::string const& fullPath = sf->ResolveFullPath(&pathError);
      if (!pathError.empty() || fullPath.empty()) {
        continue;
      }
      std::string const& extLower =
        cmSystemTools::LowerCase(sf->GetExtension());

      // Register files that will be scanned by moc or uic
      if (this->MocOrUicEnabled()) {
        if (cm->IsHeaderExtension(extLower)) {
          addMUFile(makeMUFile(sf, fullPath, true), true);
        } else if (cm->IsSourceExtension(extLower)) {
          addMUFile(makeMUFile(sf, fullPath, true), false);
        }
      }

      // Register rcc enabled files
      if (this->Rcc.Enabled) {
        if ((extLower == kw.qrc) && !sf->GetPropertyAsBool(kw.SKIP_AUTOGEN) &&
            !sf->GetPropertyAsBool(kw.SKIP_AUTORCC)) {
          // Register qrc file
          Qrc qrc;
          qrc.QrcFile = fullPath;
          qrc.QrcName =
            cmSystemTools::GetFilenameWithoutLastExtension(qrc.QrcFile);
          qrc.Generated = sf->GetIsGenerated();
          // RCC options
          {
            std::string const opts = sf->GetSafeProperty(kw.AUTORCC_OPTIONS);
            if (!opts.empty()) {
              cmExpandList(opts, qrc.Options);
            }
          }
          this->Rcc.Qrcs.push_back(std::move(qrc));
        }
      }
    }
  }
  // cmGeneratorTarget::GetConfigCommonSourceFiles computes the target's
  // sources meta data cache. Clear it so that OBJECT library targets that
  // are AUTOGEN initialized after this target get their added
  // mocs_compilation.cpp source acknowledged by this target.
  this->GenTarget->ClearSourcesCache();

  // For source files find additional headers and private headers
  if (this->MocOrUicEnabled()) {
    std::vector<MUFileHandle> extraHeaders;
    extraHeaders.reserve(this->AutogenTarget.Sources.size() * 2);
    // Header search suffixes and extensions
    static std::initializer_list<cm::string_view> const suffixes{ "", "_p" };
    auto const& exts = cm->GetHeaderExtensions();
    // Scan through sources
    for (auto const& pair : this->AutogenTarget.Sources) {
      MUFile const& muf = *pair.second;
      if (muf.MocIt || muf.UicIt) {
        // Search for the default header file and a private header
        std::string const& srcFullPath = muf.SF->ResolveFullPath();
        std::string basePath = cmStrCat(
          cmQtAutoGen::SubDirPrefix(srcFullPath),
          cmSystemTools::GetFilenameWithoutLastExtension(srcFullPath));
        for (auto const& suffix : suffixes) {
          std::string const suffixedPath = cmStrCat(basePath, suffix);
          for (auto const& ext : exts) {
            std::string fullPath = cmStrCat(suffixedPath, '.', ext);

            auto constexpr locationKind = cmSourceFileLocationKind::Known;
            cmSourceFile* sf =
              this->Makefile->GetSource(fullPath, locationKind);
            if (sf != nullptr) {
              // Check if we know about this header already
              if (cmContains(this->AutogenTarget.Headers, sf)) {
                continue;
              }
              // We only accept not-GENERATED files that do exist.
              if (!sf->GetIsGenerated() &&
                  !cmSystemTools::FileExists(fullPath)) {
                continue;
              }
            } else if (cmSystemTools::FileExists(fullPath)) {
              // Create a new source file for the existing file
              sf = this->Makefile->CreateSource(fullPath, false, locationKind);
            }

            if (sf != nullptr) {
              auto eMuf = makeMUFile(sf, fullPath, true);
              // Ony process moc/uic when the parent is processed as well
              if (!muf.MocIt) {
                eMuf->MocIt = false;
              }
              if (!muf.UicIt) {
                eMuf->UicIt = false;
              }
              extraHeaders.emplace_back(std::move(eMuf));
            }
          }
        }
      }
    }
    // Move generated files to main headers list
    for (auto& eMuf : extraHeaders) {
      addMUFile(std::move(eMuf), true);
    }
  }

  // Scan through all source files in the makefile to extract moc and uic
  // parameters.  Historically we support non target source file parameters.
  // The reason is that their file names might be discovered from source files
  // at generation time.
  if (this->MocOrUicEnabled()) {
    for (cmSourceFile* sf : this->Makefile->GetSourceFiles()) {
      // sf->GetExtension() is only valid after sf->ResolveFullPath() ...
      // Since we're iterating over source files that might be not in the
      // target we need to check for path errors (not existing files).
      std::string pathError;
      std::string const& fullPath = sf->ResolveFullPath(&pathError);
      if (!pathError.empty() || fullPath.empty()) {
        continue;
      }
      std::string const& extLower =
        cmSystemTools::LowerCase(sf->GetExtension());

      if (cm->IsHeaderExtension(extLower)) {
        if (!cmContains(this->AutogenTarget.Headers, sf)) {
          auto muf = makeMUFile(sf, fullPath, false);
          if (muf->SkipMoc || muf->SkipUic) {
            this->AutogenTarget.Headers.emplace(sf, std::move(muf));
          }
        }
      } else if (cm->IsSourceExtension(extLower)) {
        if (!cmContains(this->AutogenTarget.Headers, sf)) {
          auto muf = makeMUFile(sf, fullPath, false);
          if (muf->SkipMoc || muf->SkipUic) {
            this->AutogenTarget.Sources.emplace(sf, std::move(muf));
          }
        }
      } else if (this->Uic.Enabled && (extLower == kw.ui)) {
        // .ui file
        bool const skipAutogen = sf->GetPropertyAsBool(kw.SKIP_AUTOGEN);
        bool const skipUic =
          (skipAutogen || sf->GetPropertyAsBool(kw.SKIP_AUTOUIC));
        if (!skipUic) {
          // Check if the .ui file has uic options
          std::string const uicOpts = sf->GetSafeProperty(kw.AUTOUIC_OPTIONS);
          if (!uicOpts.empty()) {
            this->Uic.FileFiles.push_back(fullPath);
            this->Uic.FileOptions.push_back(cmExpandedList(uicOpts));
          }
        } else {
          // Register skipped .ui file
          this->Uic.SkipUi.insert(fullPath);
        }
      }
    }
  }

  // Process GENERATED sources and headers
  if (this->MocOrUicEnabled() && !this->AutogenTarget.FilesGenerated.empty()) {
    if (this->CMP0071Accept) {
      // Let the autogen target depend on the GENERATED files
      for (MUFile* muf : this->AutogenTarget.FilesGenerated) {
        this->AutogenTarget.DependFiles.insert(muf->FullPath);
      }
    } else if (this->CMP0071Warn) {
      cm::string_view property;
      if (this->Moc.Enabled && this->Uic.Enabled) {
        property = "SKIP_AUTOGEN";
      } else if (this->Moc.Enabled) {
        property = "SKIP_AUTOMOC";
      } else if (this->Uic.Enabled) {
        property = "SKIP_AUTOUIC";
      }
      std::string files;
      for (MUFile* muf : this->AutogenTarget.FilesGenerated) {
        files += cmStrCat("  ", Quoted(muf->FullPath), '\n');
      }
      this->Makefile->IssueMessage(
        MessageType::AUTHOR_WARNING,
        cmStrCat(
          cmPolicies::GetPolicyWarning(cmPolicies::CMP0071), '\n',
          "For compatibility, CMake is excluding the GENERATED source "
          "file(s):\n",
          files, "from processing by ",
          cmQtAutoGen::Tools(this->Moc.Enabled, this->Uic.Enabled, false),
          ".  If any of the files should be processed, set CMP0071 to NEW.  "
          "If any of the files should not be processed, "
          "explicitly exclude them by setting the source file property ",
          property, ":\n  set_property(SOURCE file.h PROPERTY ", property,
          " ON)\n"));
    }
  }

  // Process qrc files
  if (!this->Rcc.Qrcs.empty()) {
    const bool modernQt = (this->QtVersion.Major >= 5);
    // Target rcc options
    std::vector<std::string> optionsTarget =
      cmExpandedList(this->GenTarget->GetSafeProperty(kw.AUTORCC_OPTIONS));

    // Check if file name is unique
    for (Qrc& qrc : this->Rcc.Qrcs) {
      qrc.Unique = true;
      for (Qrc const& qrc2 : this->Rcc.Qrcs) {
        if ((&qrc != &qrc2) && (qrc.QrcName == qrc2.QrcName)) {
          qrc.Unique = false;
          break;
        }
      }
    }
    // Path checksum and file names
    {
      cmFilePathChecksum const fpathCheckSum(this->Makefile);
      for (Qrc& qrc : this->Rcc.Qrcs) {
        qrc.PathChecksum = fpathCheckSum.getPart(qrc.QrcFile);
        // RCC output file name
        qrc.RccFile = cmStrCat(this->Dir.Build, '/', qrc.PathChecksum, "/qrc_",
                               qrc.QrcName, ".cpp");
        {
          cm::string_view const baseSuffix =
            qrc.Unique ? cm::string_view() : cm::string_view(qrc.PathChecksum);
          std::string const base =
            cmStrCat(this->Dir.Info, "/RCC", qrc.QrcName, baseSuffix);
          qrc.LockFile = cmStrCat(base, ".lock");
          qrc.InfoFile = cmStrCat(base, "Info.cmake");
          qrc.SettingsFile = cmStrCat(base, "Settings.txt");
          if (this->MultiConfig) {
            for (std::string const& cfg : this->ConfigsList) {
              qrc.ConfigSettingsFile[cfg] =
                AppendFilenameSuffix(qrc.SettingsFile, "_" + cfg);
            }
          }
        }
      }
    }
    // RCC options
    for (Qrc& qrc : this->Rcc.Qrcs) {
      // Target options
      std::vector<std::string> opts = optionsTarget;
      // Merge computed "-name XYZ" option
      {
        std::string name = qrc.QrcName;
        // Replace '-' with '_'. The former is not valid for symbol names.
        std::replace(name.begin(), name.end(), '-', '_');
        if (!qrc.Unique) {
          name += cmStrCat('_', qrc.PathChecksum);
        }
        std::vector<std::string> nameOpts;
        nameOpts.emplace_back("-name");
        nameOpts.emplace_back(std::move(name));
        RccMergeOptions(opts, nameOpts, modernQt);
      }
      // Merge file option
      RccMergeOptions(opts, qrc.Options, modernQt);
      qrc.Options = std::move(opts);
    }
    // RCC resources
    for (Qrc& qrc : this->Rcc.Qrcs) {
      if (!qrc.Generated) {
        std::string error;
        RccLister const lister(this->Rcc.Executable,
                               this->Rcc.ExecutableFeatures->ListOptions);
        if (!lister.list(qrc.QrcFile, qrc.Resources, error)) {
          cmSystemTools::Error(error);
          return false;
        }
      }
    }
  }

  return true;
}

bool cmQtAutoGenInitializer::InitAutogenTarget()
{
  // Register info file as generated by CMake
  this->Makefile->AddCMakeOutputFile(this->AutogenTarget.InfoFile);

  // Files provided by the autogen target
  std::vector<std::string> autogenProvides;
  if (this->Moc.Enabled) {
    this->AddGeneratedSource(this->Moc.MocsCompilation, this->Moc, true);
    autogenProvides.push_back(this->Moc.MocsCompilation);
  }

  // Compose target comment
  std::string autogenComment;
  {
    std::string tools;
    if (this->Moc.Enabled) {
      tools += "MOC";
    }
    if (this->Uic.Enabled) {
      if (!tools.empty()) {
        tools += " and ";
      }
      tools += "UIC";
    }
    autogenComment = cmStrCat("Automatic ", tools, " for target ",
                              this->GenTarget->GetName());
  }

  // Compose command lines
  cmCustomCommandLines commandLines = cmMakeSingleCommandLine(
    { cmSystemTools::GetCMakeCommand(), "-E", "cmake_autogen",
      this->AutogenTarget.InfoFile, "$<CONFIGURATION>" });

  // Use PRE_BUILD on demand
  bool usePRE_BUILD = false;
  if (this->GlobalGen->GetName().find("Visual Studio") != std::string::npos) {
    // Under VS use a PRE_BUILD event instead of a separate target to
    // reduce the number of targets loaded into the IDE.
    // This also works around a VS 11 bug that may skip updating the target:
    //  https://connect.microsoft.com/VisualStudio/feedback/details/769495
    usePRE_BUILD = true;
  }
  // Disable PRE_BUILD in some cases
  if (usePRE_BUILD) {
    // Cannot use PRE_BUILD with file depends
    if (!this->AutogenTarget.DependFiles.empty()) {
      usePRE_BUILD = false;
    }
    // Cannot use PRE_BUILD when a global autogen target is in place
    if (AutogenTarget.GlobalTarget) {
      usePRE_BUILD = false;
    }
  }
  // Create the autogen target/command
  if (usePRE_BUILD) {
    // Add additional autogen target dependencies to origin target
    for (cmTarget* depTarget : this->AutogenTarget.DependTargets) {
      this->GenTarget->Target->AddUtility(depTarget->GetName(),
                                          this->Makefile);
    }

    // Add the pre-build command directly to bypass the OBJECT_LIBRARY
    // rejection in cmMakefile::AddCustomCommandToTarget because we know
    // PRE_BUILD will work for an OBJECT_LIBRARY in this specific case.
    //
    // PRE_BUILD does not support file dependencies!
    const std::vector<std::string> no_output;
    const std::vector<std::string> no_deps;
    cmCustomCommand cc(this->Makefile, no_output, autogenProvides, no_deps,
                       commandLines, autogenComment.c_str(),
                       this->Dir.Work.c_str());
    cc.SetEscapeOldStyle(false);
    cc.SetEscapeAllowMakeVars(true);
    this->GenTarget->Target->AddPreBuildCommand(cc);
  } else {

    // Add link library target dependencies to the autogen target
    // dependencies
    if (this->AutogenTarget.DependOrigin) {
      // add_dependencies/addUtility do not support generator expressions.
      // We depend only on the libraries found in all configs therefore.
      std::map<cmGeneratorTarget const*, std::size_t> commonTargets;
      for (std::string const& config : this->ConfigsList) {
        cmLinkImplementationLibraries const* libs =
          this->GenTarget->GetLinkImplementationLibraries(config);
        if (libs != nullptr) {
          for (cmLinkItem const& item : libs->Libraries) {
            cmGeneratorTarget const* libTarget = item.Target;
            if ((libTarget != nullptr) &&
                !StaticLibraryCycle(this->GenTarget, libTarget, config)) {
              // Increment target config count
              commonTargets[libTarget]++;
            }
          }
        }
      }
      for (auto const& item : commonTargets) {
        if (item.second == this->ConfigsList.size()) {
          this->AutogenTarget.DependTargets.insert(item.first->Target);
        }
      }
    }

    // Create autogen target
    cmTarget* autogenTarget = this->Makefile->AddUtilityCommand(
      this->AutogenTarget.Name, cmMakefile::TargetOrigin::Generator, true,
      this->Dir.Work.c_str(), /*byproducts=*/autogenProvides,
      std::vector<std::string>(this->AutogenTarget.DependFiles.begin(),
                               this->AutogenTarget.DependFiles.end()),
      commandLines, false, autogenComment.c_str());
    // Create autogen generator target
    this->LocalGen->AddGeneratorTarget(
      new cmGeneratorTarget(autogenTarget, this->LocalGen));

    // Forward origin utilities to autogen target
    if (this->AutogenTarget.DependOrigin) {
      for (BT<std::string> const& depName : this->GenTarget->GetUtilities()) {
        autogenTarget->AddUtility(depName.Value, this->Makefile);
      }
    }
    // Add additional autogen target dependencies to autogen target
    for (cmTarget* depTarget : this->AutogenTarget.DependTargets) {
      autogenTarget->AddUtility(depTarget->GetName(), this->Makefile);
    }

    // Set FOLDER property in autogen target
    if (!this->TargetsFolder.empty()) {
      autogenTarget->SetProperty("FOLDER", this->TargetsFolder.c_str());
    }

    // Add autogen target to the origin target dependencies
    this->GenTarget->Target->AddUtility(this->AutogenTarget.Name,
                                        this->Makefile);

    // Add autogen target to the global autogen target dependencies
    if (this->AutogenTarget.GlobalTarget) {
      this->GlobalInitializer->AddToGlobalAutoGen(this->LocalGen,
                                                  this->AutogenTarget.Name);
    }
  }

  return true;
}

bool cmQtAutoGenInitializer::InitRccTargets()
{
  for (Qrc const& qrc : this->Rcc.Qrcs) {
    // Register info file as generated by CMake
    this->Makefile->AddCMakeOutputFile(qrc.InfoFile);
    // Register file at target
    this->AddGeneratedSource(qrc.RccFile, this->Rcc);

    std::vector<std::string> ccOutput;
    ccOutput.push_back(qrc.RccFile);

    std::vector<std::string> ccDepends;
    // Add the .qrc and info file to the custom command dependencies
    ccDepends.push_back(qrc.QrcFile);
    ccDepends.push_back(qrc.InfoFile);

    cmCustomCommandLines commandLines;
    if (this->MultiConfig) {
      // Build for all configurations
      for (std::string const& config : this->ConfigsList) {
        commandLines.push_back(
          cmMakeCommandLine({ cmSystemTools::GetCMakeCommand(), "-E",
                              "cmake_autorcc", qrc.InfoFile, config }));
      }
    } else {
      commandLines.push_back(
        cmMakeCommandLine({ cmSystemTools::GetCMakeCommand(), "-E",
                            "cmake_autorcc", qrc.InfoFile, "$<CONFIG>" }));
    }
    std::string ccComment =
      cmStrCat("Automatic RCC for ",
               FileProjectRelativePath(this->Makefile, qrc.QrcFile));

    if (qrc.Generated || this->Rcc.GlobalTarget) {
      // Create custom rcc target
      std::string ccName;
      {
        ccName = cmStrCat(this->GenTarget->GetName(), "_arcc_", qrc.QrcName);
        if (!qrc.Unique) {
          ccName += cmStrCat('_', qrc.PathChecksum);
        }

        cmTarget* autoRccTarget = this->Makefile->AddUtilityCommand(
          ccName, cmMakefile::TargetOrigin::Generator, true,
          this->Dir.Work.c_str(), ccOutput, ccDepends, commandLines, false,
          ccComment.c_str());

        // Create autogen generator target
        this->LocalGen->AddGeneratorTarget(
          new cmGeneratorTarget(autoRccTarget, this->LocalGen));

        // Set FOLDER property in autogen target
        if (!this->TargetsFolder.empty()) {
          autoRccTarget->SetProperty("FOLDER", this->TargetsFolder.c_str());
        }
        if (!this->Rcc.ExecutableTargetName.empty()) {
          autoRccTarget->AddUtility(this->Rcc.ExecutableTargetName,
                                    this->Makefile);
        }
      }
      // Add autogen target to the origin target dependencies
      this->GenTarget->Target->AddUtility(ccName, this->Makefile);

      // Add autogen target to the global autogen target dependencies
      if (this->Rcc.GlobalTarget) {
        this->GlobalInitializer->AddToGlobalAutoRcc(this->LocalGen, ccName);
      }
    } else {
      // Create custom rcc command
      {
        std::vector<std::string> ccByproducts;

        // Add the resource files to the dependencies
        for (std::string const& fileName : qrc.Resources) {
          // Add resource file to the custom command dependencies
          ccDepends.push_back(fileName);
        }
        if (!this->Rcc.ExecutableTargetName.empty()) {
          ccDepends.push_back(this->Rcc.ExecutableTargetName);
        }
        std::string no_main_dependency;
        cmImplicitDependsList no_implicit_depends;
        this->Makefile->AddCustomCommandToOutput(
          ccOutput, ccByproducts, ccDepends, no_main_dependency,
          no_implicit_depends, commandLines, ccComment.c_str(),
          this->Dir.Work.c_str());
      }
      // Reconfigure when .qrc file changes
      this->Makefile->AddCMakeDependFile(qrc.QrcFile);
    }
  }

  return true;
}

bool cmQtAutoGenInitializer::SetupCustomTargets()
{
  // Create info directory on demand
  if (!cmSystemTools::MakeDirectory(this->Dir.Info)) {
    cmSystemTools::Error(cmStrCat("AutoGen: Could not create directory: ",
                                  Quoted(this->Dir.Info)));
    return false;
  }

  // Generate autogen target info file
  if (this->MocOrUicEnabled()) {
    // Write autogen target info files
    if (!this->SetupWriteAutogenInfo()) {
      return false;
    }
  }

  // Write AUTORCC info files
  return !this->Rcc.Enabled || this->SetupWriteRccInfo();
}

bool cmQtAutoGenInitializer::SetupWriteAutogenInfo()
{
  InfoWriter ofs(this->AutogenTarget.InfoFile);
  if (ofs) {
    // Utility lambdas
    auto MfDef = [this](const char* key) {
      return this->Makefile->GetSafeDefinition(key);
    };

    // Write common settings
    ofs.Write("# Meta\n");
    ofs.Write("AM_MULTI_CONFIG", this->MultiConfig ? "TRUE" : "FALSE");
    ofs.Write("AM_PARALLEL", this->AutogenTarget.Parallel);
    ofs.Write("AM_VERBOSITY", this->Verbosity);

    ofs.Write("# Directories\n");
    ofs.Write("AM_CMAKE_SOURCE_DIR", MfDef("CMAKE_SOURCE_DIR"));
    ofs.Write("AM_CMAKE_BINARY_DIR", MfDef("CMAKE_BINARY_DIR"));
    ofs.Write("AM_CMAKE_CURRENT_SOURCE_DIR",
              MfDef("CMAKE_CURRENT_SOURCE_DIR"));
    ofs.Write("AM_CMAKE_CURRENT_BINARY_DIR",
              MfDef("CMAKE_CURRENT_BINARY_DIR"));
    ofs.Write("AM_BUILD_DIR", this->Dir.Build);
    ofs.Write("AM_INCLUDE_DIR", this->Dir.Include);
    ofs.WriteConfig("AM_INCLUDE_DIR", this->Dir.ConfigInclude);

    std::vector<std::string> headers;
    std::vector<std::string> headersFlags;
    std::vector<std::string> headersBuildPaths;
    std::vector<std::string> sources;
    std::vector<std::string> sourcesFlags;
    std::set<std::string> moc_skip;
    std::set<std::string> uic_skip;

    // Filter headers
    {
      auto headerCount = this->AutogenTarget.Headers.size();
      headers.reserve(headerCount);
      headersFlags.reserve(headerCount);

      std::vector<MUFile const*> sortedHeaders;
      {
        sortedHeaders.reserve(headerCount);
        for (auto const& pair : this->AutogenTarget.Headers) {
          sortedHeaders.emplace_back(pair.second.get());
        }
        std::sort(sortedHeaders.begin(), sortedHeaders.end(),
                  [](MUFile const* a, MUFile const* b) {
                    return (a->FullPath < b->FullPath);
                  });
      }

      for (MUFile const* const muf : sortedHeaders) {
        if (muf->Generated && !this->CMP0071Accept) {
          continue;
        }
        if (muf->SkipMoc) {
          moc_skip.insert(muf->FullPath);
        }
        if (muf->SkipUic) {
          uic_skip.insert(muf->FullPath);
        }
        if (muf->MocIt || muf->UicIt) {
          headers.emplace_back(muf->FullPath);
          headersFlags.emplace_back(
            cmStrCat(muf->MocIt ? 'M' : 'm', muf->UicIt ? 'U' : 'u'));
        }
      }
    }
    // Header build paths
    {
      cmFilePathChecksum const fpathCheckSum(this->Makefile);
      std::unordered_set<std::string> emitted;
      for (std::string const& hdr : headers) {
        std::string const basePath =
          cmStrCat(fpathCheckSum.getPart(hdr), "/moc_",
                   cmSystemTools::GetFilenameWithoutLastExtension(hdr));
        std::string suffix;
        for (int ii = 0; ii != 1024; ++ii) {
          std::string path = cmStrCat(basePath, suffix, ".cpp");
          if (emitted.emplace(path).second) {
            headersBuildPaths.emplace_back(std::move(path));
            break;
          }
          suffix = cmStrCat('_', ii + 1);
        }
      }
    }

    // Filter sources
    {
      auto sourcesCount = this->AutogenTarget.Sources.size();
      sources.reserve(sourcesCount);
      sourcesFlags.reserve(sourcesCount);

      std::vector<MUFile const*> sorted;
      sorted.reserve(sourcesCount);
      for (auto const& pair : this->AutogenTarget.Sources) {
        sorted.emplace_back(pair.second.get());
      }
      std::sort(sorted.begin(), sorted.end(),
                [](MUFile const* a, MUFile const* b) {
                  return (a->FullPath < b->FullPath);
                });

      for (MUFile const* const muf : sorted) {
        if (muf->Generated && !this->CMP0071Accept) {
          continue;
        }
        if (muf->SkipMoc) {
          moc_skip.insert(muf->FullPath);
        }
        if (muf->SkipUic) {
          uic_skip.insert(muf->FullPath);
        }
        if (muf->MocIt || muf->UicIt) {
          sources.emplace_back(muf->FullPath);
          sourcesFlags.emplace_back(
            cmStrCat(muf->MocIt ? 'M' : 'm', muf->UicIt ? 'U' : 'u'));
        }
      }
    }

    ofs.Write("# Qt\n");
    ofs.WriteUInt("AM_QT_VERSION_MAJOR", this->QtVersion.Major);
    ofs.Write("AM_QT_MOC_EXECUTABLE", this->Moc.Executable);
    ofs.Write("AM_QT_UIC_EXECUTABLE", this->Uic.Executable);

    ofs.Write("# Files\n");
    ofs.Write("AM_CMAKE_EXECUTABLE", cmSystemTools::GetCMakeCommand());
    ofs.Write("AM_SETTINGS_FILE", this->AutogenTarget.SettingsFile);
    ofs.WriteConfig("AM_SETTINGS_FILE",
                    this->AutogenTarget.ConfigSettingsFile);
    ofs.Write("AM_PARSE_CACHE_FILE", this->AutogenTarget.ParseCacheFile);
    ofs.WriteStrings("AM_HEADERS", headers);
    ofs.WriteStrings("AM_HEADERS_FLAGS", headersFlags);
    ofs.WriteStrings("AM_HEADERS_BUILD_PATHS", headersBuildPaths);
    ofs.WriteStrings("AM_SOURCES", sources);
    ofs.WriteStrings("AM_SOURCES_FLAGS", sourcesFlags);

    // Write moc settings
    if (this->Moc.Enabled) {
      ofs.Write("# MOC settings\n");
      ofs.WriteStrings("AM_MOC_SKIP", moc_skip);
      ofs.WriteStrings("AM_MOC_DEFINITIONS", this->Moc.Defines);
      ofs.WriteConfigStrings("AM_MOC_DEFINITIONS", this->Moc.ConfigDefines);
      ofs.WriteStrings("AM_MOC_INCLUDES", this->Moc.Includes);
      ofs.WriteConfigStrings("AM_MOC_INCLUDES", this->Moc.ConfigIncludes);
      ofs.Write("AM_MOC_OPTIONS",
                this->GenTarget->GetSafeProperty("AUTOMOC_MOC_OPTIONS"));
      ofs.Write("AM_MOC_RELAXED_MODE", MfDef("CMAKE_AUTOMOC_RELAXED_MODE"));
      ofs.Write("AM_MOC_PATH_PREFIX",
                this->GenTarget->GetSafeProperty("AUTOMOC_PATH_PREFIX"));
      ofs.Write("AM_MOC_MACRO_NAMES",
                this->GenTarget->GetSafeProperty("AUTOMOC_MACRO_NAMES"));
      ofs.Write("AM_MOC_DEPEND_FILTERS",
                this->GenTarget->GetSafeProperty("AUTOMOC_DEPEND_FILTERS"));
      ofs.Write("AM_MOC_PREDEFS_CMD", this->Moc.PredefsCmd);
    }

    // Write uic settings
    if (this->Uic.Enabled) {
      // Add skipped .ui files
      uic_skip.insert(this->Uic.SkipUi.begin(), this->Uic.SkipUi.end());

      ofs.Write("# UIC settings\n");
      ofs.WriteStrings("AM_UIC_SKIP", uic_skip);
      ofs.WriteStrings("AM_UIC_TARGET_OPTIONS", this->Uic.Options);
      ofs.WriteConfigStrings("AM_UIC_TARGET_OPTIONS", this->Uic.ConfigOptions);
      ofs.WriteStrings("AM_UIC_OPTIONS_FILES", this->Uic.FileFiles);
      ofs.WriteNestedLists("AM_UIC_OPTIONS_OPTIONS", this->Uic.FileOptions);
      ofs.WriteStrings("AM_UIC_SEARCH_PATHS", this->Uic.SearchPaths);
    }
  } else {
    cmSystemTools::Error(cmStrCat("AutoGen: Could not write file ",
                                  this->AutogenTarget.InfoFile));
    return false;
  }

  return true;
}

bool cmQtAutoGenInitializer::SetupWriteRccInfo()
{
  for (Qrc const& qrc : this->Rcc.Qrcs) {
    InfoWriter ofs(qrc.InfoFile);
    if (ofs) {
      // Utility lambdas
      auto MfDef = [this](const char* key) {
        return this->Makefile->GetSafeDefinition(key);
      };

      // Write
      ofs.Write("# Configurations\n");
      ofs.Write("ARCC_MULTI_CONFIG", this->MultiConfig ? "TRUE" : "FALSE");
      ofs.Write("ARCC_VERBOSITY", this->Verbosity);
      ofs.Write("# Settings file\n");
      ofs.Write("ARCC_SETTINGS_FILE", qrc.SettingsFile);
      ofs.WriteConfig("ARCC_SETTINGS_FILE", qrc.ConfigSettingsFile);

      ofs.Write("# Directories\n");
      ofs.Write("ARCC_CMAKE_SOURCE_DIR", MfDef("CMAKE_SOURCE_DIR"));
      ofs.Write("ARCC_CMAKE_BINARY_DIR", MfDef("CMAKE_BINARY_DIR"));
      ofs.Write("ARCC_BUILD_DIR", this->Dir.Build);
      ofs.Write("ARCC_INCLUDE_DIR", this->Dir.Include);
      ofs.WriteConfig("ARCC_INCLUDE_DIR", this->Dir.ConfigInclude);

      ofs.Write("# Rcc executable\n");
      ofs.Write("ARCC_RCC_EXECUTABLE", this->Rcc.Executable);
      ofs.WriteStrings("ARCC_RCC_LIST_OPTIONS",
                       this->Rcc.ExecutableFeatures->ListOptions);

      ofs.Write("# Rcc job\n");
      ofs.Write("ARCC_LOCK_FILE", qrc.LockFile);
      ofs.Write("ARCC_SOURCE", qrc.QrcFile);
      ofs.Write("ARCC_OUTPUT_CHECKSUM", qrc.PathChecksum);
      ofs.Write("ARCC_OUTPUT_NAME",
                cmSystemTools::GetFilenameName(qrc.RccFile));
      ofs.WriteStrings("ARCC_OPTIONS", qrc.Options);
      ofs.WriteStrings("ARCC_INPUTS", qrc.Resources);
    } else {
      cmSystemTools::Error(
        cmStrCat("AutoRcc: Could not write file ", qrc.InfoFile));
      return false;
    }
  }

  return true;
}

void cmQtAutoGenInitializer::RegisterGeneratedSource(
  std::string const& filename)
{
  cmSourceFile* gFile = this->Makefile->GetOrCreateSource(filename, true);
  gFile->SetProperty("GENERATED", "1");
  gFile->SetProperty("SKIP_AUTOGEN", "1");
}

bool cmQtAutoGenInitializer::AddGeneratedSource(std::string const& filename,
                                                GenVarsT const& genVars,
                                                bool prepend)
{
  // Register source at makefile
  this->RegisterGeneratedSource(filename);
  // Add source file to target
  this->GenTarget->AddSource(filename, prepend);
  // Add source file to source group
  return this->AddToSourceGroup(filename, genVars.GenNameUpper);
}

bool cmQtAutoGenInitializer::AddToSourceGroup(std::string const& fileName,
                                              cm::string_view genNameUpper)
{
  cmSourceGroup* sourceGroup = nullptr;
  // Acquire source group
  {
    std::string property;
    std::string groupName;
    {
      // Prefer generator specific source group name
      std::initializer_list<std::string> const props{
        cmStrCat(genNameUpper, "_SOURCE_GROUP"), "AUTOGEN_SOURCE_GROUP"
      };
      for (std::string const& prop : props) {
        const char* propName =
          this->Makefile->GetState()->GetGlobalProperty(prop);
        if ((propName != nullptr) && (*propName != '\0')) {
          groupName = propName;
          property = prop;
          break;
        }
      }
    }
    // Generate a source group on demand
    if (!groupName.empty()) {
      sourceGroup = this->Makefile->GetOrCreateSourceGroup(groupName);
      if (sourceGroup == nullptr) {
        cmSystemTools::Error(
          cmStrCat(genNameUpper, " error in ", property,
                   ": Could not find or create the source group ",
                   cmQtAutoGen::Quoted(groupName)));
        return false;
      }
    }
  }
  if (sourceGroup != nullptr) {
    sourceGroup->AddGroupFile(fileName);
  }
  return true;
}

void cmQtAutoGenInitializer::AddCleanFile(std::string const& fileName)
{
  this->GenTarget->Target->AppendProperty("ADDITIONAL_CLEAN_FILES",
                                          fileName.c_str(), false);
}

static unsigned int CharPtrToUInt(const char* const input)
{
  unsigned long tmp = 0;
  if (input != nullptr && cmStrToULong(input, &tmp)) {
    return static_cast<unsigned int>(tmp);
  }
  return 0;
}

static std::vector<cmQtAutoGen::IntegerVersion> GetKnownQtVersions(
  cmGeneratorTarget const* genTarget)
{
  // Qt version variable prefixes
  static std::initializer_list<
    std::pair<cm::string_view, cm::string_view>> const keys{
    { "Qt6Core_VERSION_MAJOR", "Qt6Core_VERSION_MINOR" },
    { "Qt5Core_VERSION_MAJOR", "Qt5Core_VERSION_MINOR" },
    { "QT_VERSION_MAJOR", "QT_VERSION_MINOR" },
  };

  std::vector<cmQtAutoGen::IntegerVersion> result;
  result.reserve(keys.size() * 2);

  // Adds a version to the result (nullptr safe)
  auto addVersion = [&result](const char* major, const char* minor) {
    cmQtAutoGen::IntegerVersion ver(CharPtrToUInt(major),
                                    CharPtrToUInt(minor));
    if (ver.Major != 0) {
      result.emplace_back(ver);
    }
  };

  cmMakefile* makefile = genTarget->Makefile;

  // Read versions from variables
  for (auto const& keyPair : keys) {
    addVersion(makefile->GetDefinition(std::string(keyPair.first)),
               makefile->GetDefinition(std::string(keyPair.second)));
  }

  // Read versions from directory properties
  for (auto const& keyPair : keys) {
    addVersion(makefile->GetProperty(std::string(keyPair.first)),
               makefile->GetProperty(std::string(keyPair.second)));
  }

  return result;
}

std::pair<cmQtAutoGen::IntegerVersion, unsigned int>
cmQtAutoGenInitializer::GetQtVersion(cmGeneratorTarget const* target)
{
  std::pair<IntegerVersion, unsigned int> res(
    IntegerVersion(),
    CharPtrToUInt(target->GetLinkInterfaceDependentStringProperty(
      "QT_MAJOR_VERSION", "")));

  auto knownQtVersions = GetKnownQtVersions(target);
  if (!knownQtVersions.empty()) {
    if (res.second == 0) {
      // No specific version was requested by the target:
      // Use highest known Qt version.
      res.first = knownQtVersions.at(0);
    } else {
      // Pick a version from the known versions:
      for (auto it : knownQtVersions) {
        if (it.Major == res.second) {
          res.first = it;
          break;
        }
      }
    }
  }
  return res;
}

bool cmQtAutoGenInitializer::GetQtExecutable(GenVarsT& genVars,
                                             const std::string& executable,
                                             bool ignoreMissingTarget) const
{
  auto print_err = [this, &genVars](std::string const& err) {
    cmSystemTools::Error(cmStrCat(genVars.GenNameUpper, " for target ",
                                  this->GenTarget->GetName(), ": ", err));
  };

  // Custom executable
  {
    std::string const prop = cmStrCat(genVars.GenNameUpper, "_EXECUTABLE");
    std::string const val = this->GenTarget->Target->GetSafeProperty(prop);
    if (!val.empty()) {
      // Evaluate generator expression
      {
        cmListFileBacktrace lfbt = this->Makefile->GetBacktrace();
        cmGeneratorExpression ge(lfbt);
        std::unique_ptr<cmCompiledGeneratorExpression> cge = ge.Parse(val);
        genVars.Executable = cge->Evaluate(this->LocalGen, "");
      }
      if (genVars.Executable.empty() && !ignoreMissingTarget) {
        print_err(prop + " evaluates to an empty value");
        return false;
      }

      // Create empty compiler features.
      genVars.ExecutableFeatures =
        std::make_shared<cmQtAutoGen::CompilerFeatures>();
      return true;
    }
  }

  // Find executable target
  {
    // Find executable target name
    cm::string_view prefix;
    if (this->QtVersion.Major == 4) {
      prefix = "Qt4::";
    } else if (this->QtVersion.Major == 5) {
      prefix = "Qt5::";
    } else if (this->QtVersion.Major == 6) {
      prefix = "Qt6::";
    }
    std::string const targetName = cmStrCat(prefix, executable);

    // Find target
    cmGeneratorTarget* genTarget =
      this->LocalGen->FindGeneratorTargetToUse(targetName);
    if (genTarget != nullptr) {
      genVars.ExecutableTargetName = targetName;
      genVars.ExecutableTarget = genTarget;
      if (genTarget->IsImported()) {
        genVars.Executable = genTarget->ImportedGetLocation("");
      } else {
        genVars.Executable = genTarget->GetLocation("");
      }
    } else {
      if (ignoreMissingTarget) {
        // Create empty compiler features.
        genVars.ExecutableFeatures =
          std::make_shared<cmQtAutoGen::CompilerFeatures>();
        return true;
      }
      print_err(cmStrCat("Could not find ", executable, " executable target ",
                         targetName));
      return false;
    }
  }

  // Get executable features
  {
    std::string err;
    genVars.ExecutableFeatures = this->GlobalInitializer->GetCompilerFeatures(
      executable, genVars.Executable, err);
    if (!genVars.ExecutableFeatures) {
      print_err(err);
      return false;
    }
  }

  return true;
}
