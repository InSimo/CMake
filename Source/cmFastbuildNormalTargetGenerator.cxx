/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#include "cmFastbuildNormalTargetGenerator.h"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>

#include <cm/memory>
#include <cm/optional>
#include <cm/vector>

#include "cmComputeLinkInformation.h"
#include "cmCustomCommand.h" // IWYU pragma: keep
#include "cmCustomCommandGenerator.h"
#include "cmGeneratedFileStream.h"
#include "cmGeneratorTarget.h"
#include "cmGlobalFastbuildGenerator.h"
#include "cmLinkLineComputer.h"
#include "cmLinkLineDeviceComputer.h"
#include "cmLocalCommonGenerator.h"
#include "cmLocalGenerator.h"
#include "cmLocalFastbuildGenerator.h"
#include "cmMakefile.h"
#include "cmMessageType.h"
#include "cmFastbuildLinkLineDeviceComputer.h"
#include "cmFastbuildTypes.h"
#include "cmOSXBundleGenerator.h"
#include "cmOutputConverter.h"
#include "cmProperty.h"
#include "cmRulePlaceholderExpander.h"
#include "cmSourceFile.h"
#include "cmState.h"
#include "cmStateDirectory.h"
#include "cmStateSnapshot.h"
#include "cmStateTypes.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"

cmFastbuildNormalTargetGenerator::cmFastbuildNormalTargetGenerator(
  cmGeneratorTarget* target)
  : cmFastbuildTargetGenerator(target)
{
  if (target->GetType() != cmStateEnums::OBJECT_LIBRARY) {
    // on Windows the output dir is already needed at compile time
    // ensure the directory exists (OutDir test)
    for (auto const& config : this->GetConfigNames()) {
      this->EnsureDirectoryExists(target->GetDirectory(config));
    }
  }

  this->OSXBundleGenerator = cm::make_unique<cmOSXBundleGenerator>(target);
  this->OSXBundleGenerator->SetMacContentFolders(&this->MacContentFolders);
}

cmFastbuildNormalTargetGenerator::~cmFastbuildNormalTargetGenerator() = default;

void cmFastbuildNormalTargetGenerator::Generate(const std::string& config)
{
  std::string lang = this->GeneratorTarget->GetLinkerLanguage(config);
  if (this->TargetLinkLanguage(config).empty()) {
    cmSystemTools::Error("CMake can not determine linker language for "
                         "target: " +
                         this->GetGeneratorTarget()->GetName());
    return;
  }

  // Write the rules for each language.
  this->WriteLanguagesRules(config);

  // Write the build statements
  bool firstForConfig = true;
  for (auto const& fileConfig : this->GetConfigNames()) {
    if (!this->GetGlobalGenerator()
           ->GetCrossConfigs(fileConfig)
           .count(config)) {
      continue;
    }
    this->WriteObjectBuildStatements(config, fileConfig, firstForConfig);
    firstForConfig = false;
  }

  if (this->GetGeneratorTarget()->GetType() == cmStateEnums::OBJECT_LIBRARY) {
    this->WriteObjectLibStatement(config);
  } else {
    firstForConfig = true;
    for (auto const& fileConfig : this->GetConfigNames()) {
      if (!this->GetGlobalGenerator()
             ->GetCrossConfigs(fileConfig)
             .count(config)) {
        continue;
      }
      // If this target has cuda language link inputs, and we need to do
      // device linking
      this->WriteDeviceLinkStatement(config, fileConfig, firstForConfig);
      this->WriteLinkStatement(config, fileConfig, firstForConfig);
      firstForConfig = false;
    }
  }
  if (this->GetGlobalGenerator()->EnableCrossConfigBuild()) {
    this->GetGlobalGenerator()->AddTargetAlias(
      this->GetTargetName(), this->GetGeneratorTarget(), "all");
  }

  // For Fastbuild
  this->GetGlobalGenerator()->AddFastbuildInfoTarget(this->GetGeneratorTarget(), this->GetNameDepsTargets(config), config);
  if (this->GetGlobalGenerator()->CanTreatTargetFB(
        this->GetGeneratorTarget(),
        config)) {
    this->WriteTargetFB(config);


    // Find ADDITIONAL_CLEAN_FILES
    this->AdditionalCleanFiles(config);
  }
}

std::string cmFastbuildNormalTargetGenerator::GetCompilerId(const std::string& config)
{
  std::string lang = this->GetGeneratorTarget()->GetLinkerLanguage(config);
  std::string const& compilerId =
    this->GetMakefile()->GetSafeDefinition(cmStrCat("CMAKE_", lang, "_COMPILER_ID"));
  return compilerId;
}

std::vector<std::string> cmFastbuildNormalTargetGenerator::GetNameDepsTargets(const std::string& config)
{
  std::vector<std::string> listDeps;
  const cmFastbuildDeps depsPath = this->ComputeLinkDeps(this->TargetLinkLanguage(config), config);
  for (std::string depPath : depsPath) {
    listDeps.push_back(cmStrCat(GetNameFile(depPath), config));
  }

  return RemoveDuplicateName(listDeps);
}

std::vector<std::string> cmFastbuildNormalTargetGenerator::RemoveDuplicateName(std::vector<std::string> listName)
{
  std::vector<std::string> listUniqueName;
  std::map<std::string, std::string> mapTemp;
  for(std::string name : listName){
    mapTemp[name] = name;
  }
  for(auto name : mapTemp){
    listUniqueName.push_back(name.first);
  }
  return listUniqueName;
}

std::string cmFastbuildNormalTargetGenerator::GetNameFile(std::string namePathFile)
{
  std::string nameFile = "";
  std::size_t found = 0;
  std::size_t found1 = namePathFile.rfind('/');
  std::size_t found2 = namePathFile.rfind('\\');

  if (found1 != std::string::npos && found2 == std::string::npos)
    found = found1 + 1;
  else if (found1 == std::string::npos && found2 != std::string::npos)
    found = found2 + 1;
  else if (found1 != std::string::npos && found2 != std::string::npos) {
    if (found1 < found2)
      found = found2;
    else
      found = found1;
  }

  nameFile = namePathFile.substr(found);
  
  std::size_t found_point = nameFile.find('.');
  if (found_point != std::string::npos) {
    nameFile = nameFile.substr(0, found_point);
  }

  return nameFile;
}

std::vector<std::string>
cmFastbuildNormalTargetGenerator::GetFileDeps(std::string config)
{
  const cmFastbuildDeps implicitDeps =
    this->ComputeLinkDeps(this->TargetLinkLanguage(config), config);
  std::vector<std::string> listImplicitDeps;
  for (std::string implicitDep : implicitDeps) {
    std::size_t found_point = implicitDep.rfind('.');
    if (found_point != std::string::npos &&
        implicitDep.substr(found_point, 9) == ".manifest") {
      continue;
    }
    listImplicitDeps.push_back(implicitDep);
  }
  return RemoveDuplicateName(listImplicitDeps);
}

std::vector<std::string>
cmFastbuildNormalTargetGenerator::GetNameTargetLibraries(bool isMultiConfig,
                                                         std::string config)
{
  cmGlobalFastbuildGenerator* gfb = this->GetGlobalGenerator();
  const std::string compilerId = this->GetCompilerId(config);
  const cmFastbuildDeps implicitDeps =
    this->ComputeLinkDeps(this->TargetLinkLanguage(config), config);
  std::vector<std::string> listImplicitDeps;
  for (std::string implicitDep : implicitDeps) {
    std::string nameTarget =
      GetNameTargetLibrary(implicitDep, isMultiConfig, config);
    std::string nameInfoTargetGFB = cmStrCat(GetNameFile(implicitDep), config);

    if (compilerId == "GNU" || compilerId == "Clang"){
      // If we have a lib prefix, we remove it
      if(nameTarget.substr(0,3) == "lib"){
        nameTarget = nameTarget.substr(3);
      }
      if(nameInfoTargetGFB.substr(0,3) == "lib"){
        nameInfoTargetGFB = nameInfoTargetGFB.substr(3);
      }
    }

    // We add the dependency to the target only if it exists
    if (!nameTarget.empty() &&
        gfb->MapFastbuildInfoTargets.find(nameInfoTargetGFB) !=
          gfb->MapFastbuildInfoTargets.end())
      listImplicitDeps.push_back(nameTarget);
  }

  return RemoveDuplicateName(listImplicitDeps);
}

std::string cmFastbuildNormalTargetGenerator::GetNameTargetLibrary(
  std::string namePathFile, bool isMultiConfig, std::string config)
{
  std::string nameFile = "";
  std::string nameTarget = "";
  std::size_t found = 0;
  std::size_t found1 = namePathFile.rfind('/');
  std::size_t found2 = namePathFile.rfind('\\');

  if (found1 != std::string::npos && found2 == std::string::npos)
    found = found1 + 1;
  else if (found1 == std::string::npos && found2 != std::string::npos)
    found = found2 + 1;
  else if (found1 != std::string::npos && found2 != std::string::npos) {
    if (found1 < found2)
      found = found2;
    else
      found = found1;
  }

  nameFile = namePathFile.substr(found);
  std::size_t found_point = nameFile.rfind('.');
  if (found_point != std::string::npos) {
    nameTarget = nameFile.substr(0, found_point);
    if (nameFile.substr(found_point, 4) == ".lib" || nameFile.substr(found_point, 2) == ".a"){
      nameTarget = cmStrCat(nameTarget, "_lib");
    } else if (nameFile.find(".so") != std::string::npos) {
      nameTarget = cmStrCat(nameTarget, "_dll");
    }
    else if (nameFile.substr(found_point, 9) == ".manifest") {
      // We ignore .manifest
      return "";
    }
  }

  if (isMultiConfig) {
    nameTarget = cmStrCat(nameTarget, "_", config);
  }
  return nameTarget;
}

std::string cmFastbuildNormalTargetGenerator::GetOutputExtension(
  const std::string& lang)
{
  if (lang == "C")
    return "c";
  if (lang == "CXX")
    return "cpp";
  if (lang == "RC")
    return "rc";
  return "";
}

std::string cmFastbuildNormalTargetGenerator::GetPath(const std::string& fullPath)
{
  std::string path = "";
  auto found = fullPath.rfind("/");
  if (found != std::string::npos) {
    path = fullPath.substr(0, found+1);
  }
  return path;
}

std::string cmFastbuildNormalTargetGenerator::RemoveBackslashBeforeDoubleRib(std::string str)
{
  // We remove the "\" in "\"": CMAKE_INTDIR = \"Release\" must be "Release"
  int found = str.find("\\\"");
  while (found != std::string::npos){
    str.replace(found,1,"");
    found = str.find("\\\"");
  }
  return str;
}

std::string cmFastbuildNormalTargetGenerator::ReplaceDashWithUnderscores(
  std::string str)
{
  // Files .bff don't accept name with "-" in
  auto it = str.find("-");
  while (it != std::string::npos) {
    str.replace(it, 1, "__");
    it = str.find("-");
  }
  return str;
}

std::string cmFastbuildNormalTargetGenerator::GetGccClangCompilerOptionsFB(std::string compilerId)
{
  std::string flags = " -c \"%1\" -o \"%2\" -MD ";
  if(compilerId == "GNU") flags += " -MT \"%2\" ";
  flags += " -MF \"%2.d\" ";
  return flags;
}

std::string cmFastbuildNormalTargetGenerator::GetMsvcCompilerOptionsFB()
{
  std::string flags = " /nologo /c \"%1\" /Fo\"%2\" ";
  // if multiple CL.EXE write to the same .PDB file, please use /FS
  flags += "/FS ";
  return flags;
}

std::string cmFastbuildNormalTargetGenerator::GetRcCompilerOptionsFB()
{
  std::string flags = " /nologo /fo \"%2\" \"%1\" ";
  return flags;
}

std::string cmFastbuildNormalTargetGenerator::GetGccClangLinkOptionsFB()
{
  std::string link_flags = "\"%1\" -o \"%2\" ";
  return link_flags;
}

std::string cmFastbuildNormalTargetGenerator::GetMsvcLinkOptionsFB()
{
  std::string link_flags = "\"%1\" /OUT:\"%2\" /nologo ";
  return link_flags;
}

void cmFastbuildNormalTargetGenerator::WriteTargetFB(const std::string& config)
{
  // Complete .bff files with compilers information
  this->WriteCompileFB(config);
  cmStateEnums::TargetType targetType = this->GetGeneratorTarget()->GetType();

  // Complete .bff files to generate the targets with Fastbuild for each type of target
  if (targetType == cmStateEnums::EXECUTABLE) {
    this->WriteObjectListsFB(config, false);
    this->WriteExecutableFB(config);
  } else if (targetType == cmStateEnums::STATIC_LIBRARY) {
    this->WriteObjectListsFB(config, false);
    this->WriteLibraryFB(config);
  } else if (targetType == cmStateEnums::SHARED_LIBRARY) {
    this->WriteObjectListsFB(config, false);
    this->WriteDLLFB(config);
  } else if (targetType == cmStateEnums::MODULE_LIBRARY) {
    this->GetGlobalGenerator()->WriteSectionHeader(
      this->GetGlobalGenerator()->GetFileStream(
        config, this->GetGlobalGenerator()->IsMultiConfig()),
      cmStrCat("NOT TOTALLY AVAILABLE : MODULE LIBRARY : ",
               this->GetTargetName()));
    this->WriteObjectListsFB(config, false);
    this->WriteDLLFB(config);
  } else if (targetType == cmStateEnums::OBJECT_LIBRARY) {
    this->WriteObjectListsFB(config, true);
  } else if (targetType == cmStateEnums::GLOBAL_TARGET) {
    this->GetGlobalGenerator()->WriteSectionHeader(
      this->GetGlobalGenerator()->GetFileStream(
        config, this->GetGlobalGenerator()->IsMultiConfig()),
      cmStrCat("NOT YET AVAILABLE : GLOBAL TARGET : ", this->GetTargetName()));
  } else if (targetType == cmStateEnums::INTERFACE_LIBRARY) {
    this->GetGlobalGenerator()->WriteSectionHeader(
      this->GetGlobalGenerator()->GetFileStream(
        config, this->GetGlobalGenerator()->IsMultiConfig()),
      cmStrCat("NOT YET AVAILABLE : INTERFACE LIBRARY : ",
               this->GetTargetName()));
  } else if (targetType == cmStateEnums::UTILITY) {
    this->GetGlobalGenerator()->WriteSectionHeader(
      this->GetGlobalGenerator()->GetFileStream(
        config, this->GetGlobalGenerator()->IsMultiConfig()),
      cmStrCat("NOT YET AVAILABLE : UTILITY : ", this->GetTargetName()));
  } else if (targetType == cmStateEnums::UNKNOWN_LIBRARY) {
    this->GetGlobalGenerator()->WriteSectionHeader(
      this->GetGlobalGenerator()->GetFileStream(
        config, this->GetGlobalGenerator()->IsMultiConfig()),
      cmStrCat("NOT YET AVAILABLE : UNKNOWN LIBRARY : ",
               this->GetTargetName()));
  } else {
    this->GetGlobalGenerator()->WriteSectionHeader(
      this->GetGlobalGenerator()->GetFileStream(
        config, this->GetGlobalGenerator()->IsMultiConfig()),
      cmStrCat("NOT YET AVAILABLE : ", this->GetTargetName()));
  }

  this->GetGlobalGenerator()->TargetTreatedFinish(this->GetGeneratorTarget(), config);
}

void cmFastbuildNormalTargetGenerator::WriteCompileFB(const std::string& config)
{
  cmGlobalFastbuildGenerator* gfb = this->GetGlobalGenerator();
  cmMakefile* mf = this->GetMakefile();
  std::ostream& os = this->GetRulesFileStream();
  std::string project_name = this->GetTargetName();
  std::string lang = this->GetGeneratorTarget()->GetLinkerLanguage(config);

  std::string const& compilerId = this->GetCompilerId(config);

  std::string const& compilerVersion =
    mf->GetSafeDefinition(cmStrCat("CMAKE_", lang, "_COMPILER_VERSION"));

  // We always define the C, CXX and RC compiler if he doesn't have it,
  // because there are cases where only certain source files need it without a target having this language
  std::vector<std::string> useful_languages = {"C", "CXX", "RC"};
  for(std::string useful_language : useful_languages) {
    if (gfb->MapCompilersFB.find(useful_language) == gfb->MapCompilersFB.end()) {
      std::string executable = mf->GetSafeDefinition(
        cmStrCat("CMAKE_", useful_language, "_COMPILER"));
      if (!executable.empty()) {
        std::vector<std::string> extrafile;
        bool isCompilerCustom = false;
        std::string rootCompiler = "";
        if (useful_language == "RC")
          // The RC compiler isn't supported by Fastbuild, we must warn Fastbuild that it is a customizer compiler
          isCompilerCustom = true;
        if (useful_language == "C" || useful_language == "CXX") {
          // Information on compilers for compilation distribute
          if (compilerId == "MSVC") {
            rootCompiler = this->GetPath(mf->GetSafeDefinition(
              cmStrCat("CMAKE_", useful_language, "_COMPILER")));
            std::string architecture = mf->GetSafeDefinition(
              cmStrCat("MSVC_", useful_language, "_ARCHITECTURE_ID"));

            // VS 2019
            if (cmSystemTools::VersionCompare(cmSystemTools::OP_GREATER_EQUAL,
                                              compilerVersion.c_str(), "19.20")) {

              extrafile.push_back(gfb->Quote("$Root$/c1.dll"));
              extrafile.push_back(gfb->Quote("$Root$/c1xx.dll"));
              extrafile.push_back(gfb->Quote("$Root$/c2.dll"));
              extrafile.push_back(gfb->Quote("$Root$/atlprov.dll"));
              extrafile.push_back(gfb->Quote("$Root$/msobj140.dll"));
              extrafile.push_back(gfb->Quote("$Root$/mspdb140.dll"));
              extrafile.push_back(gfb->Quote("$Root$/mspdbcore.dll"));
              extrafile.push_back(gfb->Quote("$Root$/mspdbsrv.exe"));
              extrafile.push_back(gfb->Quote("$Root$/mspft140.dll"));
              extrafile.push_back(gfb->Quote("$Root$/msvcp140.dll"));
              extrafile.push_back(gfb->Quote("$Root$/msvcp140_atomic_wait.dll"));
              extrafile.push_back(gfb->Quote("$Root$/tbbmalloc.dll"));
              extrafile.push_back(gfb->Quote("$Root$/vcruntime140.dll"));
              extrafile.push_back(gfb->Quote("$Root$/1033/mspft140ui.dll"));
              extrafile.push_back(gfb->Quote("$Root$/1033/clui.dll"));

              if (architecture == "x64") {
                extrafile.push_back(
                  gfb->Quote("$Root$/vcruntime140_1.dll")); // Not in x86
              }
            }
            // VS 2017
            else if (cmSystemTools::VersionCompare(cmSystemTools::OP_GREATER_EQUAL,
                                              compilerVersion.c_str(), "19.10")) {
              std::vector<std::string> extrafile;
              extrafile.push_back(gfb->Quote("$Root$/c1.dll"));
              extrafile.push_back(gfb->Quote("$Root$/c1xx.dll"));
              extrafile.push_back(gfb->Quote("$Root$/c2.dll"));
              extrafile.push_back(gfb->Quote("$Root$/atlprov.dll"));
              extrafile.push_back(gfb->Quote("$Root$/msobj140.dll"));
              extrafile.push_back(gfb->Quote("$Root$/mspdb140.dll"));
              extrafile.push_back(gfb->Quote("$Root$/mspdbcore.dll"));
              extrafile.push_back(gfb->Quote("$Root$/mspdbsrv.exe"));
              extrafile.push_back(gfb->Quote("$Root$/mspft140.dll"));
              extrafile.push_back(gfb->Quote("$Root$/msvcp140.dll"));
              extrafile.push_back(gfb->Quote("$Root$/vcruntime140.dll"));
              extrafile.push_back(gfb->Quote("$Root$/1033/mspft140ui.dll"));
              extrafile.push_back(gfb->Quote("$Root$/1033/clui.dll"));
            }
            // VS 2015
            else if (cmSystemTools::VersionCompare(
                         cmSystemTools::OP_GREATER_EQUAL, compilerVersion.c_str(),
                         "19.00")) {
              std::vector<std::string> extrafile;
              extrafile.push_back(gfb->Quote("$Root$/c1.dll"));
              extrafile.push_back(gfb->Quote("$Root$/c1xx.dll"));
              extrafile.push_back(gfb->Quote("$Root$/c2.dll"));
              extrafile.push_back(gfb->Quote("$Root$/atlprov.dll"));
              extrafile.push_back(gfb->Quote("$Root$/msobj140.dll"));
              extrafile.push_back(gfb->Quote("$Root$/mspdb140.dll"));
              extrafile.push_back(gfb->Quote("$Root$/mspdbcore.dll"));
              extrafile.push_back(gfb->Quote("$Root$/mspdbsrv.exe"));
              extrafile.push_back(gfb->Quote("$Root$/mspft140.dll"));
              extrafile.push_back(gfb->Quote("$Root$/1033/clui.dll"));

              std::string redist_path = cmStrCat("$Root$/../redist/", architecture,
                                    "/Microsoft.VC140.CRT");
              if (architecture == "x64") {
                redist_path = cmStrCat("$Root$/../../redist/", architecture,
                                       "/Microsoft.VC140.CRT");
              }
              extrafile.push_back(
                gfb->Quote(cmStrCat(redist_path, "/msvcp140.dll")));
              extrafile.push_back(
                gfb->Quote(cmStrCat(redist_path, "/vccorlib140.dll")));
              extrafile.push_back(
                gfb->Quote(cmStrCat(redist_path, "/vcruntime140.dll")));
            }
          }
        }

        // Write in file .bff the compiler
        gfb->WriteSectionHeader(
          os, cmStrCat("Compiler ", gfb->Quote(useful_language)));
        gfb->WriteCommand(os, "Compiler", gfb->Quote(useful_language));
        gfb->WritePushScope(os);
        gfb->WriteVariableFB(os, "Executable", gfb->Quote(executable));
        if(isCompilerCustom) gfb->WriteVariableFB(os, "CompilerFamily", gfb->Quote("custom"));
        if(!rootCompiler.empty())gfb->WriteVariableFB(os, "Root", gfb->Quote(rootCompiler));
        if(!extrafile.empty()) gfb->WriteArray(os, "ExtraFiles", extrafile);
        gfb->WritePopScope(os);

        // We backup the treated compiler
        gfb->MapCompilersFB[useful_language] = "";
      }
    }
  }

  if (gfb->MapCompilersFB.find(lang) == gfb->MapCompilersFB.end()) {
    // The compiler isn't yet defined for this lang
    std::string executable =
      mf->GetSafeDefinition(cmStrCat("CMAKE_", lang, "_COMPILER"));

    // Write in file .bff the compiler
    gfb->WriteSectionHeader(os, cmStrCat("Compiler ", gfb->Quote(lang)));
    gfb->WriteCommand(os, "Compiler", gfb->Quote(lang));
    gfb->WritePushScope(os);
    gfb->WriteVariableFB(os, "Executable", gfb->Quote(executable));
    gfb->WritePopScope(os);

    // We backup the treated compiler
    gfb->MapCompilersFB[lang] = "";
  }

  std::string flags = " ";
  std::string create_static_library = mf->GetSafeDefinition("CMAKE_AR");

  std::string linker = mf->GetSafeDefinition("CMAKE_LINKER");
  std::string link_flags = " ";
  std::string librarian_flags = " ";

  if (lang == "RC") {
    flags = this->GetRcCompilerOptionsFB();
    link_flags = this->GetMsvcLinkOptionsFB(); // The RC linker is the MSVC linker
    librarian_flags = link_flags;
  }
  else if (compilerId == "MSVC") {
    flags = this->GetMsvcCompilerOptionsFB();
    link_flags = this->GetMsvcLinkOptionsFB();
    librarian_flags = link_flags;
  }
  else if(compilerId == "GNU" || compilerId == "Clang"){
    flags = this->GetGccClangCompilerOptionsFB(compilerId);
    link_flags = this->GetGccClangLinkOptionsFB();
    librarian_flags = "qc \"%2\" \"%1\"";
  }
  link_flags += mf->GetSafeDefinition("LINK_OPTIONS");

  // Write in file .bff the compiler information for this target
  gfb->WriteSectionHeader(os, "Info Compilers");
  gfb->WriteVariableFB(
    os,
    cmStrCat("Compiler", lang, config, this->ReplaceDashWithUnderscores(project_name)),
                       "");
  gfb->WritePushScopeStruct(os);
  gfb->WriteVariableFB(os, "Compiler", gfb->Quote(lang));
  gfb->WriteVariableFB(os, "CompilerOptions", gfb->Quote(flags));
  gfb->WriteVariableFB(os, "Librarian", gfb->Quote(create_static_library));
  gfb->WriteVariableFB(os, "LibrarianOptions", gfb->Quote(librarian_flags));
  gfb->WriteVariableFB(os, "Linker", gfb->Quote(linker));
  gfb->WriteVariableFB(os, "LinkerOptions", gfb->Quote(link_flags));
  gfb->WritePopScope(os);
}

std::string cmFastbuildNormalTargetGenerator::GetUiFileDependencies(const std::string& config) {
  // We check if there are .ui files to generate
  std::string ui_deps = "";
  std::vector<cmSourceFile const*> headerSources;
  this->GeneratorTarget->GetHeaderSources(headerSources, config);
  for(auto sa : headerSources){
    std::string file_name = this->GetNameFile(sa->GetFullPath());
    if( file_name.substr(0, 3) == "ui_"){
      // Match the names of custom commands with 'Exec' from .ui
      ui_deps += this->GetGlobalGenerator()->Quote(cmStrCat(file_name, config));
    }
  }
  return ui_deps;
}

void cmFastbuildNormalTargetGenerator::WriteObjectListsFB(const std::string& config, bool isObjectLibrary)
{
  cmGlobalFastbuildGenerator* gfb = this->GetGlobalGenerator();
  std::ostream& os = gfb->GetFileStream(config, gfb->IsMultiConfig());
  std::string language = this->GetGeneratorTarget()->GetLinkerLanguage(config);
  std::string const& compilerId = this->GetCompilerId(config);

  std::vector<cmSourceFile const*> objectSources;
  this->GeneratorTarget->GetObjectSources(objectSources, config);
  std::string target_output =
    this->GetGeneratorTarget()->GetObjectDirectory(config);
  std::string target_name = this->GetTargetName();

  std::string section_header_name;
  std::string objectList_name;
  std::string short_name = this->GetShortOutputName(config);

  // Determine the different names to use
  if (!gfb->IsMultiConfig()) {
    section_header_name = target_name;
    objectList_name = cmStrCat(short_name, "_obj");
  } else {
    section_header_name = cmStrCat(target_name, " : ", config);
    objectList_name = cmStrCat(short_name, "_obj_", config);
  }

  gfb->WriteSectionHeader(os, section_header_name);

  std::string compilerOptions = "";
  if (!objectSources.empty()) {
    compilerOptions =
      this->ComputeFlagsForObject(objectSources[0], language, config);
    std::string defines = cmStrCat(
      " ", this->ComputeDefines(objectSources[0], language, config), " ");
    if (compilerId == "GNU" || compilerId == "Clang"){
      // We remove the "\" which can cause problem example: CMAKE_INTDIR = \"Release\" must be "Release"
      defines = this->RemoveBackslashBeforeDoubleRib(defines);
    }
    compilerOptions += defines;
    compilerOptions +=
      this->ComputeIncludes(objectSources[0], language, config);
  }

  std::vector<std::string> output_objects;
  this->GetGeneratorTarget()->GetTargetObjectNames(config, output_objects);

  // For differentiated output path
  int nbSourceFile = 0;
  std::string output_object_path;
  if (!objectSources.empty())
    output_object_path =
      this->GetPath(this->GetPath(output_objects[0]));
  std::string output_object_path_temp;

  // For differentiated compile options
  int nbObjectList = 1;
  std::string compilerOptionsTemp = "";

  // For differentiated files with differentely extension
  std::string extension;
  std::string extension_temp;
  if (!objectSources.empty())
    extension = objectSources[0]->GetExtension();

  std::vector<std::string> objectList;
  for (cmSourceFile const* sf : objectSources) {
    output_object_path_temp = this->GetPath(output_objects[nbSourceFile]);
    extension_temp = sf->GetExtension();
    nbSourceFile++;
    compilerOptionsTemp =
      this->ComputeFlagsForObject(sf, language, config);
    std::string defines = cmStrCat(
      " ", this->ComputeDefines(sf, language, config), " ");
    if (compilerId == "GNU" || compilerId == "Clang"){
      // We remove the "\" which can cause problem example: CMAKE_INTDIR = \"Release\" must be "Release"
      defines = this->RemoveBackslashBeforeDoubleRib(defines);
    }
    compilerOptionsTemp += defines;
    compilerOptionsTemp +=
      this->ComputeIncludes(sf, language, config);

    // We check if we can process this source file with the previous ones or not
    if (compilerOptions != compilerOptionsTemp ||
        output_object_path != output_object_path_temp ||
        extension_temp != extension) {
      // We process a sub-list of the source files of this target
      std::string under_objectList_name =
        cmStrCat(objectList_name, "_", std::to_string(nbObjectList));
      this->WriteObjectListFB(config, language, under_objectList_name,
                              objectList, compilerOptions, output_object_path, extension);
      objectList.clear();
      nbObjectList++;
    }
    compilerOptions = compilerOptionsTemp;
    output_object_path = output_object_path_temp;
    extension = extension_temp;
    // We add this source file in the sub-list of source files to be processed soon
    objectList.push_back(cmStrCat("\"", sf->GetFullPath(), "\""));
  }

  // We process the last sub-list of source files in the global ObjectList of this target
  std::string under_objectList_name =
    cmStrCat(objectList_name, "_", std::to_string(nbObjectList));
  this->WriteObjectListFB(config, language, under_objectList_name, objectList,
                          compilerOptions, output_object_path, extension);
  
  // Gives the information to write the corresponding Fastbuild function later
  gfb->AddTargetsFastbuildToWrite("Obj", under_objectList_name, "", "",
                                  this->GetUiFileDependencies(config), config);

  // Alias
  std::string under_objectLists = gfb->Quote(under_objectList_name);
  for (int i = 1; i < nbObjectList; i++) {
    under_objectLists +=
      gfb->Quote(cmStrCat(objectList_name, "_", std::to_string(i)));
  }
  if (isObjectLibrary){
    bool excludeFromAll = false;
    gfb->AddTargetAliasFB(gfb->Quote(cmStrCat(objectList_name, "_deps")),
                          gfb->Quote(objectList_name),
                          config, excludeFromAll);
  }
}

void cmFastbuildNormalTargetGenerator::WriteObjectListFB(
  const std::string& config, const std::string& language,
  const std::string& under_objectList_name,
  std::vector<std::string> objectList, const std::string& compilerOptions,
  const std::string& output_path, const std::string& SourceFileExtension)
{
  cmGlobalFastbuildGenerator* gfb = this->GetGlobalGenerator();
  std::ostream& os = gfb->GetFileStream(config, gfb->IsMultiConfig());
  std::string const& compilerId = this->GetCompilerId(config);

  std::string object_output = cmStrCat(
    this->GetGeneratorTarget()->GetObjectDirectory(config), output_path);
  std::string extension = ".obj";
  if (compilerId == "GNU" || compilerId == "Clang") extension = ".o";
  std::string lang = language;
  if (language == "RC" || SourceFileExtension == "rc") {
    extension = ".res";
    lang = "RC";
  }

  std::string var_info_compile = "";
  std::string compiler = "";
  std::string compiler_options = "";
  if (language != "RC" && SourceFileExtension == "rc") {
    // Get the rc compiler if we must compile rc files without this language at the target
    lang = "RC";
    compiler = lang;
    compiler_options = this->GetRcCompilerOptionsFB();
  } else {
    if (language != "C" && SourceFileExtension == "c") {
      // Get the C compiler if we must compile C files without this language at the target
      lang = "C";
      compiler = lang;
      if (compilerId == "MSVC") compiler_options = this->GetMsvcCompilerOptionsFB();
      if (compilerId == "GNU" || compilerId == "Clang") compiler_options = this->GetGccClangCompilerOptionsFB(compilerId);
    }
    else if (language != "CXX" && SourceFileExtension == "cpp") {
      // Get the CXX compiler if we must compile CXX files without this language at the target
      lang = "CXX";
      compiler = lang;
      if (compilerId == "MSVC")
        compiler_options = this->GetMsvcCompilerOptionsFB();
      if (compilerId == "GNU" || compilerId == "Clang")
        compiler_options = this->GetGccClangCompilerOptionsFB(compilerId);
    }
    else {
      // Obtain the information compilers corresponding to the language of the target
      var_info_compile =
        cmStrCat(".Compiler", lang, config,
                 this->ReplaceDashWithUnderscores(this->GetTargetName()));
    }
  }

  // Determine the output extension
  std::string extension_lang = this->GetOutputExtension(lang);
  if (!extension_lang.empty())
    extension = cmStrCat(".", extension_lang, extension);

  // Write in file .bff for create info objects files
  gfb->WriteVariableFB(os, cmStrCat("Obj", under_objectList_name),
                       "");
  gfb->WritePushScopeStruct(os);
  if(!compiler.empty()) gfb->WriteVariableFB(os, "Compiler", gfb->Quote(compiler));
  if (!var_info_compile.empty())
    gfb->WriteCommand(os, "Using", var_info_compile);
  if (!compiler_options.empty())
    gfb->WriteVariableFB(os, "CompilerOptions", gfb->Quote(compiler_options));
  if (!compilerOptions.empty() && lang != "RC")
    gfb->WriteVariableFB(os, "CompilerOptions", gfb->Quote(compilerOptions),
                         "+");
  gfb->WriteArray(os, "CompilerInputFiles", objectList);
  gfb->WriteVariableFB(os, "CompilerOutputPath", gfb->Quote(object_output));
  gfb->WriteVariableFB(os, "CompilerOutputExtension", gfb->Quote(extension));
  gfb->WritePopScope(os);
}

void cmFastbuildNormalTargetGenerator::GetTargetFlagsFB(
  const std::string& config, std::string& linkLibs, std::string& flags,
  std::string& linkFlags)
{
  cmGlobalFastbuildGenerator* gfb = this->GetGlobalGenerator();
  std::ostream& os = gfb->GetFileStream(config, gfb->IsMultiConfig());

  std::string createRule = this->GetGeneratorTarget()->GetCreateRuleVariable(
    this->TargetLinkLanguage(config), config);
  bool useWatcomQuote =
    this->GetMakefile()->IsOn(createRule + "_USE_WATCOM_QUOTE");

  cmLocalFastbuildGenerator& localGen = *this->GetLocalGenerator();

  std::unique_ptr<cmLinkLineComputer> linkLineComputer =
    gfb->CreateLinkLineComputer(
      this->GetLocalGenerator(),
      this->GetLocalGenerator()->GetStateSnapshot().GetDirectory());
  linkLineComputer->SetUseWatcomQuote(useWatcomQuote);
  linkLineComputer->SetUseFastbuildMulti(gfb->IsMultiConfig());
  std::string frameworkPath;
  std::string linkPath;
  localGen.GetTargetFlags(linkLineComputer.get(), config, linkLibs, flags,
                          linkFlags, frameworkPath, linkPath,
                          this->GetGeneratorTarget());
}

std::string cmFastbuildNormalTargetGenerator::GetLinkFlagsFB(
  const std::string& config, const std::string& language, std::string target_name)
{
  std::string const& compilerId = this->GetCompilerId(config);
  std::string linkLibs;
  std::string flags;
  std::string linkFlags;
  this->GetTargetFlagsFB(config, linkLibs, flags, linkFlags);

  cmMakefile* mf = this->GetMakefile();
  std::string cmake_arguments = "";
  std::string pdb =
    cmStrCat(this->GetGeneratorTarget()->GetPDBDirectory(config), "/",
             this->GetGeneratorTarget()->GetPDBName(config));

  auto output_info = this->GetGeneratorTarget()->GetOutputInfo(config);
  std::string output_name;
  if (compilerId == "GNU" || compilerId == "Clang") {
    output_name = cmStrCat(this->GetShortOutputName(config), ".a");
  } else {
    output_name = cmStrCat(this->GetShortOutputName(config), ".lib");
  }
  std::string implib = cmStrCat(output_info->ImpDir, "/", output_name);

  // Create the suite of link options
  if(compilerId == "MSVC"){
    cmake_arguments += "-E vs_link_exe ";
    cmake_arguments +=
      cmStrCat(" --intdir=", this->GetGeneratorTarget()->GetSupportDirectory());
    cmake_arguments += " --rc=";
    cmake_arguments += cmOutputConverter::EscapeForCMake(
      mf->GetSafeDefinition("CMAKE_RC_COMPILER"));
    cmake_arguments += " --mt=";
    cmake_arguments +=
      cmOutputConverter::EscapeForCMake(mf->GetSafeDefinition("CMAKE_MT"));
    cmake_arguments +=
      cmStrCat(" --manifests ", this->GetManifests(config));
    cmake_arguments += " -- ";
    cmake_arguments +=
      cmOutputConverter::EscapeForCMake(mf->GetSafeDefinition("CMAKE_LINKER"));
    cmake_arguments += " /nologo $FB_INPUT_1_PLACEHOLDER$"; // %1
    cmake_arguments += " /out:$FB_INPUT_2_PLACEHOLDER$";    // %2
    cmake_arguments +=
      cmStrCat(" /implib:", implib);
    if (!pdb.empty())
      cmake_arguments += cmStrCat(" /pdb:", pdb);
    cmake_arguments += cmStrCat(" ", linkFlags, " ");
    cmake_arguments += linkLibs;
  }
  else if (compilerId == "GNU" || compilerId == "Clang"){
    cmake_arguments += " $FB_INPUT_1_PLACEHOLDER$";      // %1
    cmake_arguments += " -o $FB_INPUT_2_PLACEHOLDER$ ";  // %2
    //cmake_arguments += cmStrCat(" -l ", implib);
    cmake_arguments += cmStrCat(" ", linkFlags, " ");
    cmake_arguments += linkLibs;
  }

  return cmake_arguments;
}

std::string cmFastbuildNormalTargetGenerator::GetShortOutputName(
  const std::string& config)
{
  cmGlobalFastbuildGenerator* gfb = this->GetGlobalGenerator();
  std::string full_name = this->GetGeneratorTarget()->GetFullName(config);
  std::string output_name = full_name;
  int found = full_name.find(".");
  if(found != std::string::npos){
    output_name = full_name.substr(0, found);
  }
  return this->ReplaceDashWithUnderscores(output_name);
}

void cmFastbuildNormalTargetGenerator::WriteExecutableFB(
  const std::string& config)
{
  cmGlobalFastbuildGenerator* gfb = this->GetGlobalGenerator();
  std::ostream& os = gfb->GetFileStream(config, gfb->IsMultiConfig());
  bool isMultiConfig = gfb->IsMultiConfig();
  std::string language = this->TargetLinkLanguage(config);
  std::string const& compilerId = this->GetCompilerId(config);
  cmMakefile* mf = this->GetMakefile();

  std::string target_output = this->GetTargetOutputDir(config);
  std::string target_name = this->GetTargetName();
  std::string executable_name;
  std::string objectList_name;
  std::string alias_name;
  std::string short_name = this->GetShortOutputName(config);

  // Determine the different names to use
  if (!isMultiConfig) {
    executable_name = cmStrCat(short_name, "_exe");
    objectList_name = cmStrCat(short_name, "_obj");
    alias_name = cmStrCat(short_name, "_exe_deps");
  } else {
    executable_name = cmStrCat(short_name, "_exe_", config);
    objectList_name = cmStrCat(short_name, "_obj_", config);
    alias_name = cmStrCat(short_name, "_exe_", config, "_deps");
  }
  std::string output_full_name =
    this->GetGeneratorTarget()->GetFullName(config);

  // Get the targets dependencies
  std::vector<std::string> targetDeps = GetNameTargetLibraries(isMultiConfig, config);
  std::string listTargetDeps = "";
  for (std::string targetDep : targetDeps) {
    listTargetDeps +=
      gfb->Quote(targetDep);
  }

  // Get the information for the link
  std::string linker_command = mf->GetSafeDefinition("CMAKE_COMMAND");
  std::string executable_extension = ".exe";
  if (compilerId == "GNU" || compilerId == "Clang") {
    linker_command = mf->GetSafeDefinition(cmStrCat("CMAKE_", language, "_COMPILER"));
    executable_extension = "";
  }
  std::string arguments = this->GetLinkFlagsFB(config, language, target_name);

  // Write in file .bff for create info executable
  gfb->WriteVariableFB(os, cmStrCat("Exe", executable_name), "");
  gfb->WritePushScopeStruct(os);
  gfb->WriteCommand(os, "Using",
                    cmStrCat(".Compiler", language, config,
                             this->ReplaceDashWithUnderscores(this->GetTargetName())));
  gfb->WriteVariableFB(os, "Linker", gfb->Quote(linker_command));
  gfb->WriteVariableFB(
    os, "LinkerOutput",
    gfb->Quote(cmStrCat(target_output, "/", output_full_name)));
  gfb->WriteVariableFB(os, "LinkerOptions", gfb->Quote(arguments));
  gfb->WritePopScope(os);

  // Gives the information to write the corresponding Fastbuild function later
  gfb->AddTargetsFastbuildToWrite("Exe", executable_name, objectList_name,
                                  listTargetDeps,
                                  cmStrCat(this->GetUiFileDependencies(config), listTargetDeps), config);

  // Alias
  std::vector<std::string> implicitDeps =
    GetNameTargetLibraries(isMultiConfig, config);
  std::string listImplicitDepsAlias = "";
  for (std::string implicitDep : implicitDeps) {
    if (!implicitDep.empty())
      listImplicitDepsAlias += gfb->Quote(cmStrCat(implicitDep, "_deps"));
  }
  std::string listDeps = listImplicitDepsAlias;
  bool excludeFromAll = false;
  listDeps += gfb->Quote(executable_name);
  gfb->AddTargetAliasFB(gfb->Quote(alias_name), listDeps, config, excludeFromAll);

  if ((compilerId == "MSVC" && !isMultiConfig) ||
      (isMultiConfig && gfb->GetDefaultFileConfig() == config)) {
    // Have the good name alias for success cmake basic test with MSVC or
    // Have the good name alias for success cmake basic test with Multi-Config
    gfb->AddTargetAliasFB(gfb->Quote(target_name), gfb->Quote(executable_name),
                          config, true);
  }
}

void cmFastbuildNormalTargetGenerator::WriteLibraryFB(
  const std::string& config)
{
  cmGlobalFastbuildGenerator* gfb = this->GetGlobalGenerator();
  std::ostream& os = gfb->GetFileStream(config, gfb->IsMultiConfig());
  bool isMultiConfig = gfb->IsMultiConfig();
  cmMakefile* mf = this->GetMakefile();

  std::string language = this->TargetLinkLanguage(config);
  std::string target_name = this->GetTargetName();

  std::string const& compilerId =
    mf->GetSafeDefinition(cmStrCat("CMAKE_", language, "_COMPILER_ID"));

  auto output_info = this->GetGeneratorTarget()->GetOutputInfo(config);
  std::string librarian = "";
  std::string librarianOptions = "";
  std::string output_name;
  if (this->GetGeneratorTarget()->GetType() == cmStateEnums::STATIC_LIBRARY) {
    output_name = this->GetGeneratorTarget()->GetFullName(config);
  } else {
    if (compilerId == "GNU" || compilerId == "Clang") {
      output_name = cmStrCat(this->GetShortOutputName(config), ".a");
    }
    else {
      output_name = cmStrCat(this->GetShortOutputName(config), ".lib");
    }
  }
  std::string library_output =
    cmStrCat(output_info->ImpDir, "/", output_name);

  std::string short_name = this->GetShortOutputName(config);
  std::string library_name;
  std::string objectList_name;
  std::string alias_name;

  // Determine the different names to use
  std::string suffix_lib = "lib";
  if (this->GetGeneratorTarget()->GetType() != cmStateEnums::STATIC_LIBRARY)
    suffix_lib = "slib";

  if (!isMultiConfig) {
    library_name = cmStrCat(short_name, "_", suffix_lib);
    objectList_name = cmStrCat(short_name, "_obj");
    alias_name = cmStrCat(short_name, "_", suffix_lib, "_deps");
  } else {
    library_name = cmStrCat(short_name, "_", suffix_lib, "_", config);
    objectList_name = cmStrCat(short_name, "_obj_", config);
    alias_name = cmStrCat(short_name, "_", suffix_lib, "_", config, "_deps");
  }

  // Get the targets dependencies
  std::vector<std::string> targetDeps = GetNameTargetLibraries(isMultiConfig, config);
  std::string listTargetDeps = "";
  for (std::string targetDep : targetDeps) {
    listTargetDeps += gfb->Quote(targetDep);
  }

  // Write in file .bff for create info static library
  gfb->WriteVariableFB(os, cmStrCat("Lib", library_name), "");
  gfb->WritePushScopeStruct(os);
  gfb->WriteCommand(os, "Using",
                    cmStrCat(".Compiler", language, config,
                             this->ReplaceDashWithUnderscores(this->GetTargetName())));
  gfb->WriteVariableFB(
    os, "LibrarianOutput", gfb->Quote(library_output));
  if(!librarian.empty()){
    gfb->WriteVariableFB(
      os, "Librarian", gfb->Quote(librarian));
  }
  if(!librarianOptions.empty()){
    gfb->WriteVariableFB(
      os, "LibrarianOptions", gfb->Quote(librarianOptions));
  }
  gfb->WritePopScope(os);

  // Gives the information to write the corresponding Fastbuild function later
  gfb->AddTargetsFastbuildToWrite(
    "Lib", library_name, gfb->Quote(objectList_name), listTargetDeps, this->GetUiFileDependencies(config), config);

  // Alias
  if (this->GetGeneratorTarget()->GetType() == cmStateEnums::STATIC_LIBRARY) {
    std::vector<std::string> implicitDeps =
      GetNameTargetLibraries(isMultiConfig, config);
    std::string listImplicitDeps = "";
    for (std::string implicitDep : implicitDeps) {
      listImplicitDeps += gfb->Quote(implicitDep);
    }
    std::string listDeps = listImplicitDeps;
    listDeps += gfb->Quote(library_name);
    bool excludeFromAll = false;
    gfb->AddTargetAliasFB(gfb->Quote(alias_name), listDeps, config,
                          excludeFromAll);
  }
}

void cmFastbuildNormalTargetGenerator::WriteDLLFB(const std::string& config)
{
  cmGlobalFastbuildGenerator* gfb = this->GetGlobalGenerator();
  std::ostream& os = gfb->GetFileStream(config, gfb->IsMultiConfig());
  bool isMultiConfig = gfb->IsMultiConfig();
  cmMakefile* mf = this->GetMakefile();

  std::string language = this->TargetLinkLanguage(config);
  std::string const& compilerId =
    this->GetMakefile()->GetSafeDefinition(cmStrCat("CMAKE_", language, "_COMPILER_ID"));
  std::string target_output = this->GetTargetOutputDir(config);
  std::string target_name = this->GetTargetName();

  std::string short_name = this->GetShortOutputName(config);
  std::string library_name;
  std::string objectList_name;
  std::string dll_name;
  std::string alias_name;
  
  // Determine the different names to use
  if (!isMultiConfig) {
    objectList_name = cmStrCat(short_name, "_obj");
    library_name = cmStrCat(short_name, "_slib");
    dll_name = cmStrCat(short_name, "_lib");
    alias_name = cmStrCat(short_name, "_lib-deps");
  } else {
    objectList_name = cmStrCat(short_name, "_obj-", config);
    library_name = cmStrCat(short_name, "_slib_", config);
    dll_name = cmStrCat(short_name, "_lib_", config);
    alias_name = cmStrCat(short_name, "_lib_", config, "_deps");
  }

  // Write in file .bff for create static library
  this->WriteLibraryFB(config);
  
  auto output_info = this->GetGeneratorTarget()->GetOutputInfo(config);
  std::string implib;
  std::string extension_lib;
  std::string output_name = this->GetShortOutputName(config);
  if (compilerId == "GNU" || compilerId == "Clang") {
    extension_lib = ".a";
  } else {
    extension_lib = ".lib";
  }
  implib = cmStrCat(output_info->ImpDir, "/", output_name, extension_lib);

  std::string output_full_name =
    this->GetGeneratorTarget()->GetFullName(config);
  std::string output_dll = cmStrCat(target_output, "/", output_full_name);

  std::string linker_command = mf->GetSafeDefinition("CMAKE_COMMAND");
  if (compilerId == "GNU" || compilerId == "Clang") {
    linker_command = mf->GetSafeDefinition(cmStrCat("CMAKE_", language, "_COMPILER"));
  }
  std::string arguments = this->GetLinkFlagsFB(config, language, target_name);
  if (compilerId == "MSVC"){
    arguments += " /dll";
  }
  else if (compilerId == "GNU" || compilerId == "Clang"){
    arguments += " -shared";
  }

  // Get the targets dependencies
  std::vector<std::string> implicitDeps =
    GetNameTargetLibraries(isMultiConfig, config);
  std::string listImplicitDeps = "";
  for (std::string implicitDep : implicitDeps) {
    listImplicitDeps += gfb->Quote(implicitDep);
  }
  std::string listDeps = listImplicitDeps;
  
  // Write in file .bff for create info dynamic library
  gfb->WriteVariableFB(os, cmStrCat("Dll", dll_name), "");
  gfb->WritePushScopeStruct(os);
  gfb->WriteVariableFB(os, "Linker", gfb->Quote(linker_command));
  gfb->WriteVariableFB(os, "LinkerOptions", gfb->Quote(arguments));
  gfb->WriteVariableFB(
    os, "LinkerOutput",
    gfb->Quote(output_dll));
  gfb->WritePopScope(os);

  // Gives the information to write the corresponding Fastbuild function later
  gfb->AddTargetsFastbuildToWrite(
    "Dll", dll_name, library_name, "",
    cmStrCat(this->GetUiFileDependencies(config), listDeps, gfb->Quote(library_name)), config);

  // Alias
  bool excludeFromAll = false;
  listDeps += gfb->Quote(dll_name);
  gfb->AddTargetAliasFB(gfb->Quote(alias_name), listDeps, config, excludeFromAll);
}


void cmFastbuildNormalTargetGenerator::WriteLanguagesRules(
  const std::string& config)
{
#ifdef FASTBUILD_GEN_VERBOSE_FILES
  cmGlobalFastbuildGenerator::WriteDivider(this->GetRulesFileStream());
  this->GetRulesFileStream()
    << "// Rules for each languages for "
    << cmState::GetTargetTypeName(this->GetGeneratorTarget()->GetType())
    << " target " << this->GetTargetName() << "\n\n";
#endif

  // Write rules for languages compiled in this target.
  std::set<std::string> languages;
  std::vector<cmSourceFile const*> sourceFiles;
  this->GetGeneratorTarget()->GetObjectSources(sourceFiles, config);
  for (cmSourceFile const* sf : sourceFiles) {
    std::string const lang = sf->GetLanguage();
    if (!lang.empty()) {
      languages.insert(lang);
    }
  }
  for (std::string const& language : languages) {
    this->WriteLanguageRules(language, config);
  }
}

const char* cmFastbuildNormalTargetGenerator::GetVisibleTypeName() const
{
  switch (this->GetGeneratorTarget()->GetType()) {
    case cmStateEnums::STATIC_LIBRARY:
      return "static library";
    case cmStateEnums::SHARED_LIBRARY:
      return "shared library";
    case cmStateEnums::MODULE_LIBRARY:
      if (this->GetGeneratorTarget()->IsCFBundleOnApple()) {
        return "CFBundle shared module";
      } else {
        return "shared module";
      }
    case cmStateEnums::EXECUTABLE:
      return "executable";
    default:
      return nullptr;
  }
}

std::string cmFastbuildNormalTargetGenerator::LanguageLinkerRule(
  const std::string& config) const
{
  return cmStrCat(
    this->TargetLinkLanguage(config), "_",
    cmState::GetTargetTypeName(this->GetGeneratorTarget()->GetType()),
    "_LINKER__",
    cmGlobalFastbuildGenerator::EncodeRuleName(
      this->GetGeneratorTarget()->GetName()),
    "_", config);
}

std::string cmFastbuildNormalTargetGenerator::LanguageLinkerDeviceRule(
  const std::string& config) const
{
  return cmStrCat(
    this->TargetLinkLanguage(config), "_",
    cmState::GetTargetTypeName(this->GetGeneratorTarget()->GetType()),
    "_DEVICE_LINKER__",
    cmGlobalFastbuildGenerator::EncodeRuleName(
      this->GetGeneratorTarget()->GetName()),
    "_", config);
}

std::string cmFastbuildNormalTargetGenerator::LanguageLinkerCudaDeviceRule(
  const std::string& config) const
{
  return cmStrCat(
    this->TargetLinkLanguage(config), "_DEVICE_LINK__",
    cmGlobalFastbuildGenerator::EncodeRuleName(this->GeneratorTarget->GetName()),
    '_', config);
}

std::string cmFastbuildNormalTargetGenerator::LanguageLinkerCudaDeviceCompileRule(
  const std::string& config) const
{
  return cmStrCat(
    this->TargetLinkLanguage(config), "_DEVICE_LINK_COMPILE__",
    cmGlobalFastbuildGenerator::EncodeRuleName(this->GeneratorTarget->GetName()),
    '_', config);
}

std::string cmFastbuildNormalTargetGenerator::LanguageLinkerCudaFatbinaryRule(
  const std::string& config) const
{
  return cmStrCat(
    this->TargetLinkLanguage(config), "_FATBINARY__",
    cmGlobalFastbuildGenerator::EncodeRuleName(this->GeneratorTarget->GetName()),
    '_', config);
}

struct cmFastbuildRemoveNoOpCommands
{
  bool operator()(std::string const& cmd)
  {
    return cmd.empty() || cmd[0] == ':';
  }
};

void cmFastbuildNormalTargetGenerator::WriteNvidiaDeviceLinkRule(
  bool useResponseFile, const std::string& config)
{
  cmFastbuildRule rule(this->LanguageLinkerDeviceRule(config));
  if (!this->GetGlobalGenerator()->HasRule(rule.Name)) {
    cmRulePlaceholderExpander::RuleVariables vars;
    vars.CMTargetName = this->GetGeneratorTarget()->GetName().c_str();
    vars.CMTargetType =
      cmState::GetTargetTypeName(this->GetGeneratorTarget()->GetType())
        .c_str();

    vars.Language = "CUDA";

    // build response file name
    std::string responseFlag = this->GetMakefile()->GetSafeDefinition(
      "CMAKE_CUDA_RESPONSE_FILE_DEVICE_LINK_FLAG");

    if (!useResponseFile || responseFlag.empty()) {
      vars.Objects = "$in";
      vars.LinkLibraries = "$LINK_PATH $LINK_LIBRARIES";
    } else {
      rule.RspFile = "$RSP_FILE";
      responseFlag += rule.RspFile;

      // build response file content
      if (this->GetGlobalGenerator()->IsGCCOnWindows()) {
        rule.RspContent = "$in";
      } else {
        rule.RspContent = "$in_newline";
      }
      rule.RspContent += " $LINK_LIBRARIES";
      vars.Objects = responseFlag.c_str();
      vars.LinkLibraries = "";
    }

    vars.ObjectDir = "$OBJECT_DIR";

    vars.Target = "$TARGET_FILE";

    vars.SONameFlag = "$SONAME_FLAG";
    vars.TargetSOName = "$SONAME";
    vars.TargetPDB = "$TARGET_PDB";
    vars.TargetCompilePDB = "$TARGET_COMPILE_PDB";

    vars.Flags = "$FLAGS";
    vars.LinkFlags = "$LINK_FLAGS";
    vars.Manifests = "$MANIFESTS";

    vars.LanguageCompileFlags = "$LANGUAGE_COMPILE_FLAGS";

    std::string launcher;
    cmProp val = this->GetLocalGenerator()->GetRuleLauncher(
      this->GetGeneratorTarget(), "RULE_LAUNCH_LINK");
    if (cmNonempty(val)) {
      launcher = cmStrCat(*val, ' ');
    }

    std::unique_ptr<cmRulePlaceholderExpander> rulePlaceholderExpander(
      this->GetLocalGenerator()->CreateRulePlaceholderExpander());

    // Rule for linking library/executable.
    std::vector<std::string> linkCmds = this->ComputeDeviceLinkCmd();
    for (std::string& linkCmd : linkCmds) {
      linkCmd = cmStrCat(launcher, linkCmd);
      rulePlaceholderExpander->ExpandRuleVariables(this->GetLocalGenerator(),
                                                   linkCmd, vars);
    }

    // If there is no ranlib the command will be ":".  Skip it.
    cm::erase_if(linkCmds, cmFastbuildRemoveNoOpCommands());

    rule.Command =
      this->GetLocalGenerator()->BuildCommandLine(linkCmds, config, config);

    // Write the linker rule with response file if needed.
    rule.Comment =
      cmStrCat("Rule for linking ", this->TargetLinkLanguage(config), ' ',
               this->GetVisibleTypeName(), '.');
    rule.Description =
      cmStrCat("Linking ", this->TargetLinkLanguage(config), ' ',
               this->GetVisibleTypeName(), " $TARGET_FILE");
    rule.Restat = "$RESTAT";

    this->GetGlobalGenerator()->AddRule(rule);
  }
}

void cmFastbuildNormalTargetGenerator::WriteDeviceLinkRules(
  const std::string& config)
{
  const cmMakefile* mf = this->GetMakefile();

  cmFastbuildRule rule(this->LanguageLinkerCudaDeviceRule(config));
  rule.Command = this->GetLocalGenerator()->BuildCommandLine(
    { cmStrCat(mf->GetRequiredDefinition("CMAKE_CUDA_DEVICE_LINKER"),
               " -arch=$ARCH $REGISTER -o=$out $in") },
    config, config);
  rule.Comment = "Rule for CUDA device linking.";
  rule.Description = "Linking CUDA $out";
  this->GetGlobalGenerator()->AddRule(rule);

  cmRulePlaceholderExpander::RuleVariables vars;
  vars.CMTargetName = this->GetGeneratorTarget()->GetName().c_str();
  vars.CMTargetType =
    cmState::GetTargetTypeName(this->GetGeneratorTarget()->GetType()).c_str();

  vars.Language = "CUDA";
  vars.Object = "$out";
  vars.Fatbinary = "$FATBIN";
  vars.RegisterFile = "$REGISTER";

  std::string flags = this->GetFlags("CUDA", config);
  vars.Flags = flags.c_str();

  std::string compileCmd = this->GetMakefile()->GetRequiredDefinition(
    "CMAKE_CUDA_DEVICE_LINK_COMPILE");
  std::unique_ptr<cmRulePlaceholderExpander> rulePlaceholderExpander(
    this->GetLocalGenerator()->CreateRulePlaceholderExpander());
  rulePlaceholderExpander->ExpandRuleVariables(this->GetLocalGenerator(),
                                               compileCmd, vars);

  rule.Name = this->LanguageLinkerCudaDeviceCompileRule(config);
  rule.Command = this->GetLocalGenerator()->BuildCommandLine({ compileCmd },
                                                             config, config);
  rule.Comment = "Rule for compiling CUDA device stubs.";
  rule.Description = "Compiling CUDA device stub $out";
  this->GetGlobalGenerator()->AddRule(rule);

  rule.Name = this->LanguageLinkerCudaFatbinaryRule(config);
  rule.Command = this->GetLocalGenerator()->BuildCommandLine(
    { cmStrCat(mf->GetRequiredDefinition("CMAKE_CUDA_FATBINARY"),
               " -64 -cmdline=--compile-only -compress-all -link "
               "--embedded-fatbin=$out $PROFILES") },
    config, config);
  rule.Comment = "Rule for CUDA fatbinaries.";
  rule.Description = "Creating fatbinary $out";
  this->GetGlobalGenerator()->AddRule(rule);
}

void cmFastbuildNormalTargetGenerator::WriteLinkRule(bool useResponseFile,
                                                 const std::string& config)
{
  cmStateEnums::TargetType targetType = this->GetGeneratorTarget()->GetType();

  std::string linkRuleName = this->LanguageLinkerRule(config);
  if (!this->GetGlobalGenerator()->HasRule(linkRuleName)) {
    cmFastbuildRule rule(std::move(linkRuleName));
    cmRulePlaceholderExpander::RuleVariables vars;
    vars.CMTargetName = this->GetGeneratorTarget()->GetName().c_str();
    vars.CMTargetType = cmState::GetTargetTypeName(targetType).c_str();

    std::string lang = this->TargetLinkLanguage(config);
    vars.Language = config.c_str();
    vars.AIXExports = "$AIX_EXPORTS";

    if (this->TargetLinkLanguage(config) == "Swift") {
      vars.SwiftLibraryName = "$SWIFT_LIBRARY_NAME";
      vars.SwiftModule = "$SWIFT_MODULE";
      vars.SwiftModuleName = "$SWIFT_MODULE_NAME";
      vars.SwiftOutputFileMap = "$SWIFT_OUTPUT_FILE_MAP";
      vars.SwiftSources = "$SWIFT_SOURCES";

      vars.Defines = "$DEFINES";
      vars.Flags = "$FLAGS";
      vars.Includes = "$INCLUDES";
    }

    std::string responseFlag;

    std::string cmakeVarLang =
      cmStrCat("CMAKE_", this->TargetLinkLanguage(config));

    // build response file name
    std::string cmakeLinkVar = cmakeVarLang + "_RESPONSE_FILE_LINK_FLAG";
    cmProp flag = this->GetMakefile()->GetDefinition(cmakeLinkVar);

    if (flag) {
      responseFlag = *flag;
    } else {
      responseFlag = "@";
    }

    if (!useResponseFile || responseFlag.empty()) {
      vars.Objects = "$in";
      vars.LinkLibraries = "$LINK_PATH $LINK_LIBRARIES";
    } else {
      rule.RspFile = "$RSP_FILE";
      responseFlag += rule.RspFile;

      // build response file content
      if (this->GetGlobalGenerator()->IsGCCOnWindows()) {
        rule.RspContent = "$in";
      } else {
        rule.RspContent = "$in_newline";
      }
      rule.RspContent += " $LINK_PATH $LINK_LIBRARIES";
      if (this->TargetLinkLanguage(config) == "Swift") {
        vars.SwiftSources = responseFlag.c_str();
      } else {
        vars.Objects = responseFlag.c_str();
      }
      vars.LinkLibraries = "";
    }

    vars.ObjectDir = "$OBJECT_DIR";

    vars.Target = "$TARGET_FILE";

    vars.SONameFlag = "$SONAME_FLAG";
    vars.TargetSOName = "$SONAME";
    vars.TargetInstallNameDir = "$INSTALLNAME_DIR";
    vars.TargetPDB = "$TARGET_PDB";

    // Setup the target version.
    std::string targetVersionMajor;
    std::string targetVersionMinor;
    {
      std::ostringstream majorStream;
      std::ostringstream minorStream;
      int major;
      int minor;
      this->GetGeneratorTarget()->GetTargetVersion(major, minor);
      majorStream << major;
      minorStream << minor;
      targetVersionMajor = majorStream.str();
      targetVersionMinor = minorStream.str();
    }
    vars.TargetVersionMajor = targetVersionMajor.c_str();
    vars.TargetVersionMinor = targetVersionMinor.c_str();

    vars.Flags = "$FLAGS";
    vars.LinkFlags = "$LINK_FLAGS";
    vars.Manifests = "$MANIFESTS";

    std::string langFlags;
    if (targetType != cmStateEnums::EXECUTABLE) {
      langFlags += "$LANGUAGE_COMPILE_FLAGS $ARCH_FLAGS";
      vars.LanguageCompileFlags = langFlags.c_str();
    }

    std::string launcher;
    cmProp val = this->GetLocalGenerator()->GetRuleLauncher(
      this->GetGeneratorTarget(), "RULE_LAUNCH_LINK");
    if (cmNonempty(val)) {
      launcher = cmStrCat(*val, ' ');
    }

    std::unique_ptr<cmRulePlaceholderExpander> rulePlaceholderExpander(
      this->GetLocalGenerator()->CreateRulePlaceholderExpander());

    // Rule for linking library/executable.
    std::vector<std::string> linkCmds = this->ComputeLinkCmd(config);
    for (std::string& linkCmd : linkCmds) {
      linkCmd = cmStrCat(launcher, linkCmd);
      rulePlaceholderExpander->ExpandRuleVariables(this->GetLocalGenerator(),
                                                   linkCmd, vars);
    }

    // If there is no ranlib the command will be ":".  Skip it.
    cm::erase_if(linkCmds, cmFastbuildRemoveNoOpCommands());

    linkCmds.insert(linkCmds.begin(), "$PRE_LINK");
    linkCmds.emplace_back("$POST_BUILD");
    rule.Command =
      this->GetLocalGenerator()->BuildCommandLine(linkCmds, config, config);

    // Write the linker rule with response file if needed.
    rule.Comment =
      cmStrCat("Rule for linking ", this->TargetLinkLanguage(config), ' ',
               this->GetVisibleTypeName(), '.');
    rule.Description =
      cmStrCat("Linking ", this->TargetLinkLanguage(config), ' ',
               this->GetVisibleTypeName(), " $TARGET_FILE");
    rule.Restat = "$RESTAT";
    this->GetGlobalGenerator()->AddRule(rule);
  }

  auto const tgtNames = this->TargetNames(config);
  if (tgtNames.Output != tgtNames.Real &&
      !this->GetGeneratorTarget()->IsFrameworkOnApple()) {
    std::string cmakeCommand =
      this->GetLocalGenerator()->ConvertToOutputFormat(
        cmSystemTools::GetCMakeCommand(), cmOutputConverter::SHELL);
    if (targetType == cmStateEnums::EXECUTABLE) {
      cmFastbuildRule rule("CMAKE_SYMLINK_EXECUTABLE");
      {
        std::vector<std::string> cmd;
        cmd.push_back(cmakeCommand + " -E cmake_symlink_executable $in $out");
        cmd.emplace_back("$POST_BUILD");
        rule.Command =
          this->GetLocalGenerator()->BuildCommandLine(cmd, config, config);
      }
      rule.Description = "Creating executable symlink $out";
      rule.Comment = "Rule for creating executable symlink.";
      this->GetGlobalGenerator()->AddRule(rule);
    } else {
      cmFastbuildRule rule("CMAKE_SYMLINK_LIBRARY");
      {
        std::vector<std::string> cmd;
        cmd.push_back(cmakeCommand +
                      " -E cmake_symlink_library $in $SONAME $out");
        cmd.emplace_back("$POST_BUILD");
        rule.Command =
          this->GetLocalGenerator()->BuildCommandLine(cmd, config, config);
      }
      rule.Description = "Creating library symlink $out";
      rule.Comment = "Rule for creating library symlink.";
      this->GetGlobalGenerator()->AddRule(rule);
    }
  }
}

std::vector<std::string> cmFastbuildNormalTargetGenerator::ComputeDeviceLinkCmd()
{
  std::vector<std::string> linkCmds;

  // this target requires separable cuda compilation
  // now build the correct command depending on if the target is
  // an executable or a dynamic library.
  std::string linkCmd;
  switch (this->GetGeneratorTarget()->GetType()) {
    case cmStateEnums::STATIC_LIBRARY:
    case cmStateEnums::SHARED_LIBRARY:
    case cmStateEnums::MODULE_LIBRARY: {
      this->GetMakefile()->GetDefExpandList("CMAKE_CUDA_DEVICE_LINK_LIBRARY",
                                            linkCmds);
    } break;
    case cmStateEnums::EXECUTABLE: {
      this->GetMakefile()->GetDefExpandList(
        "CMAKE_CUDA_DEVICE_LINK_EXECUTABLE", linkCmds);
    } break;
    default:
      break;
  }
  return linkCmds;
}

std::vector<std::string> cmFastbuildNormalTargetGenerator::ComputeLinkCmd(
  const std::string& config)
{
  std::vector<std::string> linkCmds;
  cmMakefile* mf = this->GetMakefile();
  {
    // If we have a rule variable prefer it. In the case of static libraries
    // this occurs when things like IPO is enabled, and we need to use the
    // CMAKE_<lang>_CREATE_STATIC_LIBRARY_IPO define instead.
    std::string linkCmdVar = this->GetGeneratorTarget()->GetCreateRuleVariable(
      this->TargetLinkLanguage(config), config);
    cmProp linkCmd = mf->GetDefinition(linkCmdVar);
    if (linkCmd) {
      std::string linkCmdStr = *linkCmd;
      if (this->GetGeneratorTarget()->HasImplibGNUtoMS(config)) {
        std::string ruleVar =
          cmStrCat("CMAKE_", this->GeneratorTarget->GetLinkerLanguage(config),
                   "_GNUtoMS_RULE");
        if (cmProp rule = this->Makefile->GetDefinition(ruleVar)) {
          linkCmdStr += *rule;
        }
      }
      cmExpandList(linkCmdStr, linkCmds);
      if (this->GetGeneratorTarget()->GetPropertyAsBool("LINK_WHAT_YOU_USE")) {
        std::string cmakeCommand = cmStrCat(
          this->GetLocalGenerator()->ConvertToOutputFormat(
            cmSystemTools::GetCMakeCommand(), cmLocalGenerator::SHELL),
          " -E __run_co_compile --lwyu=");
        cmGeneratorTarget& gt = *this->GetGeneratorTarget();
        std::string targetOutputReal = this->ConvertToFastbuildPath(
          gt.GetFullPath(config, cmStateEnums::RuntimeBinaryArtifact,
                         /*realname=*/true));
        cmakeCommand += targetOutputReal;
        linkCmds.push_back(std::move(cmakeCommand));
      }
      return linkCmds;
    }
  }
  switch (this->GetGeneratorTarget()->GetType()) {
    case cmStateEnums::STATIC_LIBRARY: {
      // We have archive link commands set. First, delete the existing archive.
      {
        std::string cmakeCommand =
          this->GetLocalGenerator()->ConvertToOutputFormat(
            cmSystemTools::GetCMakeCommand(), cmOutputConverter::SHELL);
        linkCmds.push_back(cmakeCommand + " -E rm -f $TARGET_FILE");
      }
      // TODO: Use ARCHIVE_APPEND for archives over a certain size.
      {
        std::string linkCmdVar = cmStrCat(
          "CMAKE_", this->TargetLinkLanguage(config), "_ARCHIVE_CREATE");

        linkCmdVar = this->GeneratorTarget->GetFeatureSpecificLinkRuleVariable(
          linkCmdVar, this->TargetLinkLanguage(config), config);

        std::string const& linkCmd = mf->GetRequiredDefinition(linkCmdVar);
        cmExpandList(linkCmd, linkCmds);
      }
      {
        std::string linkCmdVar = cmStrCat(
          "CMAKE_", this->TargetLinkLanguage(config), "_ARCHIVE_FINISH");

        linkCmdVar = this->GeneratorTarget->GetFeatureSpecificLinkRuleVariable(
          linkCmdVar, this->TargetLinkLanguage(config), config);

        std::string const& linkCmd = mf->GetRequiredDefinition(linkCmdVar);
        cmExpandList(linkCmd, linkCmds);
      }
#ifdef __APPLE__
      // On macOS ranlib truncates the fractional part of the static archive
      // file modification time.  If the archive and at least one contained
      // object file were created within the same second this will make look
      // the archive older than the object file. On subsequent ninja runs this
      // leads to re-achiving and updating dependent targets.
      // As a work-around we touch the archive after ranlib (see #19222).
      {
        std::string cmakeCommand =
          this->GetLocalGenerator()->ConvertToOutputFormat(
            cmSystemTools::GetCMakeCommand(), cmOutputConverter::SHELL);
        linkCmds.push_back(cmakeCommand + " -E touch $TARGET_FILE");
      }
#endif
    } break;
    case cmStateEnums::SHARED_LIBRARY:
    case cmStateEnums::MODULE_LIBRARY:
      break;
    case cmStateEnums::EXECUTABLE:
      if (this->TargetLinkLanguage(config) == "Swift") {
        if (this->GeneratorTarget->IsExecutableWithExports()) {
          this->Makefile->GetDefExpandList("CMAKE_EXE_EXPORTS_Swift_FLAG",
                                           linkCmds);
        }
      }
      break;
    default:
      assert(false && "Unexpected target type");
  }
  return linkCmds;
}

void cmFastbuildNormalTargetGenerator::WriteDeviceLinkStatement(
  const std::string& config, const std::string& fileConfig,
  bool firstForConfig)
{
  cmGlobalFastbuildGenerator* globalGen = this->GetGlobalGenerator();
  if (!globalGen->GetLanguageEnabled("CUDA")) {
    return;
  }

  cmGeneratorTarget* genTarget = this->GetGeneratorTarget();

  bool requiresDeviceLinking = requireDeviceLinking(
    *this->GeneratorTarget, *this->GetLocalGenerator(), config);
  if (!requiresDeviceLinking) {
    return;
  }

  // First and very important step is to make sure while inside this
  // step our link language is set to CUDA
  std::string const& objExt =
    this->Makefile->GetSafeDefinition("CMAKE_CUDA_OUTPUT_EXTENSION");

  std::string targetOutputDir =
    cmStrCat(this->GetLocalGenerator()->GetTargetDirectory(genTarget),
             globalGen->ConfigDirectory(config), "/");
  targetOutputDir = globalGen->ExpandCFGIntDir(targetOutputDir, config);

  std::string targetOutputReal =
    this->ConvertToFastbuildPath(targetOutputDir + "cmake_device_link" + objExt);

  if (firstForConfig) {
    globalGen->GetByproductsForCleanTarget(config).push_back(targetOutputReal);
  }
  this->DeviceLinkObject = targetOutputReal;

  // Write comments.
  cmGlobalFastbuildGenerator::WriteDivider(this->GetCommonFileStream());
  this->GetCommonFileStream()
    << "// Device Link build statements for "
    << cmState::GetTargetTypeName(genTarget->GetType()) << " target "
    << this->GetTargetName() << "\n\n";

  if (this->Makefile->GetSafeDefinition("CMAKE_CUDA_COMPILER_ID") == "Clang") {
    std::string architecturesStr =
      this->GeneratorTarget->GetSafeProperty("CUDA_ARCHITECTURES");

    if (cmIsOff(architecturesStr)) {
      this->Makefile->IssueMessage(MessageType::FATAL_ERROR,
                                   "CUDA_SEPARABLE_COMPILATION on Clang "
                                   "requires CUDA_ARCHITECTURES to be set.");
      return;
    }

    this->WriteDeviceLinkRules(config);
    this->WriteDeviceLinkStatements(config, cmExpandedList(architecturesStr),
                                    targetOutputReal);
  } else {
    this->WriteNvidiaDeviceLinkStatement(config, fileConfig, targetOutputDir,
                                         targetOutputReal);
  }
}

void cmFastbuildNormalTargetGenerator::WriteDeviceLinkStatements(
  const std::string& config, const std::vector<std::string>& architectures,
  const std::string& output)
{
  // Ensure there are no duplicates.
  const cmFastbuildDeps explicitDeps = [&]() -> std::vector<std::string> {
    std::unordered_set<std::string> depsSet;
    const cmFastbuildDeps linkDeps =
      this->ComputeLinkDeps(this->TargetLinkLanguage(config), config, true);
    const cmFastbuildDeps objects = this->GetObjects(config);
    depsSet.insert(linkDeps.begin(), linkDeps.end());
    depsSet.insert(objects.begin(), objects.end());

    std::vector<std::string> deps;
    std::copy(depsSet.begin(), depsSet.end(), std::back_inserter(deps));
    return deps;
  }();

  const std::string objectDir =
    cmStrCat(this->GeneratorTarget->GetSupportDirectory(),
             this->GetGlobalGenerator()->ConfigDirectory(config));
  const std::string fastbuildOutputDir = this->ConvertToFastbuildPath(objectDir);

  cmFastbuildBuild fatbinary(this->LanguageLinkerCudaFatbinaryRule(config));

  // Link device code for each architecture.
  for (const std::string& architectureKind : architectures) {
    // Clang always generates real code, so strip the specifier.
    const std::string architecture =
      architectureKind.substr(0, architectureKind.find('-'));
    const std::string cubin =
      cmStrCat(fastbuildOutputDir, "/sm_", architecture, ".cubin");

    fatbinary.Variables["PROFILES"] +=
      cmStrCat(" -im=profile=sm_", architecture, ",file=", cubin);
    fatbinary.ExplicitDeps.emplace_back(cubin);

    cmFastbuildBuild dlink(this->LanguageLinkerCudaDeviceRule(config));
    dlink.ExplicitDeps = explicitDeps;
    dlink.Outputs = { cubin };
    dlink.Variables["ARCH"] = cmStrCat("sm_", architecture);

    // The generated register file contains macros that when expanded register
    // the device routines. Because the routines are the same for all
    // architectures the register file will be the same too. Thus generate it
    // only on the first invocation to reduce overhead.
    if (fatbinary.ExplicitDeps.size() == 1) {
      dlink.Variables["REGISTER"] = cmStrCat(
        "--register-link-binaries=", fastbuildOutputDir, "/cmake_cuda_register.h");
    }

    this->GetGlobalGenerator()->WriteBuild(this->GetCommonFileStream(), dlink);
  }

  // Combine all architectures into a single fatbinary.
  fatbinary.Outputs = { cmStrCat(fastbuildOutputDir, "/cmake_cuda_fatbin.h") };
  this->GetGlobalGenerator()->WriteBuild(this->GetCommonFileStream(),
                                         fatbinary);

  // Compile the stub that registers the kernels and contains the fatbinaries.
  cmFastbuildBuild dcompile(this->LanguageLinkerCudaDeviceCompileRule(config));
  dcompile.Outputs = { output };
  dcompile.ExplicitDeps = { cmStrCat(fastbuildOutputDir, "/cmake_cuda_fatbin.h") };
  dcompile.Variables["FATBIN"] =
    this->GetLocalGenerator()->ConvertToOutputFormat(
      cmStrCat(objectDir, "/cmake_cuda_fatbin.h"), cmOutputConverter::SHELL);
  dcompile.Variables["REGISTER"] =
    this->GetLocalGenerator()->ConvertToOutputFormat(
      cmStrCat(objectDir, "/cmake_cuda_register.h"), cmOutputConverter::SHELL);
  this->GetGlobalGenerator()->WriteBuild(this->GetCommonFileStream(),
                                         dcompile);
}

void cmFastbuildNormalTargetGenerator::WriteNvidiaDeviceLinkStatement(
  const std::string& config, const std::string& fileConfig,
  const std::string& outputDir, const std::string& output)
{
  cmGeneratorTarget* genTarget = this->GetGeneratorTarget();
  cmGlobalFastbuildGenerator* globalGen = this->GetGlobalGenerator();

  std::string targetOutputImplib = this->ConvertToFastbuildPath(
    genTarget->GetFullPath(config, cmStateEnums::ImportLibraryArtifact));

  if (config != fileConfig) {
    std::string targetOutputFileConfigDir =
      cmStrCat(this->GetLocalGenerator()->GetTargetDirectory(genTarget),
               globalGen->ConfigDirectory(fileConfig), "/");
    targetOutputFileConfigDir =
      globalGen->ExpandCFGIntDir(outputDir, fileConfig);
    if (outputDir == targetOutputFileConfigDir) {
      return;
    }

    if (!genTarget->GetFullName(config, cmStateEnums::ImportLibraryArtifact)
           .empty() &&
        !genTarget
           ->GetFullName(fileConfig, cmStateEnums::ImportLibraryArtifact)
           .empty() &&
        targetOutputImplib ==
          this->ConvertToFastbuildPath(genTarget->GetFullPath(
            fileConfig, cmStateEnums::ImportLibraryArtifact))) {
      return;
    }
  }

  // Compute the comment.
  cmFastbuildBuild build(this->LanguageLinkerDeviceRule(config));
  build.Comment =
    cmStrCat("Link the ", this->GetVisibleTypeName(), ' ', output);

  cmFastbuildVars& vars = build.Variables;

  // Compute outputs.
  build.Outputs.push_back(output);
  // Compute specific libraries to link with.
  build.ExplicitDeps = this->GetObjects(config);
  build.ImplicitDeps =
    this->ComputeLinkDeps(this->TargetLinkLanguage(config), config);

  std::string frameworkPath;
  std::string linkPath;

  std::string createRule =
    genTarget->GetCreateRuleVariable(this->TargetLinkLanguage(config), config);
  const bool useWatcomQuote =
    this->GetMakefile()->IsOn(createRule + "_USE_WATCOM_QUOTE");
  cmLocalFastbuildGenerator& localGen = *this->GetLocalGenerator();

  vars["TARGET_FILE"] =
    localGen.ConvertToOutputFormat(output, cmOutputConverter::SHELL);

  std::unique_ptr<cmLinkLineComputer> linkLineComputer(
    new cmFastbuildLinkLineDeviceComputer(
      this->GetLocalGenerator(),
      this->GetLocalGenerator()->GetStateSnapshot().GetDirectory(),
      globalGen));
  linkLineComputer->SetUseWatcomQuote(useWatcomQuote);
  linkLineComputer->SetUseFastbuildMulti(globalGen->IsMultiConfig());

  localGen.GetDeviceLinkFlags(linkLineComputer.get(), config,
                              vars["LINK_LIBRARIES"], vars["LINK_FLAGS"],
                              frameworkPath, linkPath, genTarget);

  this->addPoolFastbuildVariable("JOB_POOL_LINK", genTarget, vars);

  vars["LINK_FLAGS"] = globalGen->EncodeLiteral(vars["LINK_FLAGS"]);

  vars["MANIFESTS"] = this->GetManifests(config);

  vars["LINK_PATH"] = frameworkPath + linkPath;

  // Compute language specific link flags.
  std::string langFlags;
  localGen.AddLanguageFlagsForLinking(langFlags, genTarget, "CUDA", config);
  vars["LANGUAGE_COMPILE_FLAGS"] = langFlags;

  auto const tgtNames = this->TargetNames(config);
  if (genTarget->HasSOName(config)) {
    vars["SONAME_FLAG"] =
      this->GetMakefile()->GetSONameFlag(this->TargetLinkLanguage(config));
    vars["SONAME"] = tgtNames.SharedObject;
    if (genTarget->GetType() == cmStateEnums::SHARED_LIBRARY) {
      std::string install_dir =
        this->GetGeneratorTarget()->GetInstallNameDirForBuildTree(config);
      if (!install_dir.empty()) {
        vars["INSTALLNAME_DIR"] = localGen.ConvertToOutputFormat(
          install_dir, cmOutputConverter::SHELL);
      }
    }
  }

  if (!tgtNames.ImportLibrary.empty()) {
    const std::string impLibPath = localGen.ConvertToOutputFormat(
      targetOutputImplib, cmOutputConverter::SHELL);
    vars["TARGET_IMPLIB"] = impLibPath;
    this->EnsureParentDirectoryExists(impLibPath);
  }

  const std::string objPath =
    cmStrCat(this->GetGeneratorTarget()->GetSupportDirectory(),
             globalGen->ConfigDirectory(config));

  vars["OBJECT_DIR"] = this->GetLocalGenerator()->ConvertToOutputFormat(
    this->ConvertToFastbuildPath(objPath), cmOutputConverter::SHELL);
  this->EnsureDirectoryExists(objPath);

  this->SetMsvcTargetPdbVariable(vars, config);

  std::string& linkLibraries = vars["LINK_LIBRARIES"];
  std::string& link_path = vars["LINK_PATH"];
  if (globalGen->IsGCCOnWindows()) {
    // ar.exe can't handle backslashes in rsp files (implicitly used by gcc)
    std::replace(linkLibraries.begin(), linkLibraries.end(), '\\', '/');
    std::replace(link_path.begin(), link_path.end(), '\\', '/');
  }

  // Device linking currently doesn't support response files so
  // do not check if the user has explicitly forced a response file.
  int const commandLineLengthLimit =
    static_cast<int>(cmSystemTools::CalculateCommandLineLengthLimit()) -
    globalGen->GetRuleCmdLength(build.Rule);

  build.RspFile = this->ConvertToFastbuildPath(
    cmStrCat("CMakeFiles/", genTarget->GetName(),
             globalGen->IsMultiConfig() ? cmStrCat('.', config) : "", ".rsp"));

  // Gather order-only dependencies.
  this->GetLocalGenerator()->AppendTargetDepends(
    this->GetGeneratorTarget(), build.OrderOnlyDeps, config, config,
    DependOnTargetArtifactFB);

  // Write the build statement for this target.
  bool usedResponseFile = false;
  globalGen->WriteBuild(this->GetCommonFileStream(), build,
                        commandLineLengthLimit, &usedResponseFile);
  this->WriteNvidiaDeviceLinkRule(usedResponseFile, config);
}

void cmFastbuildNormalTargetGenerator::WriteLinkStatement(
  const std::string& config, const std::string& fileConfig,
  bool firstForConfig)
{
  cmMakefile* mf = this->GetMakefile();
  cmGlobalFastbuildGenerator* globalGen = this->GetGlobalGenerator();
  cmGeneratorTarget* gt = this->GetGeneratorTarget();

  std::string targetOutput = this->ConvertToFastbuildPath(gt->GetFullPath(config));
  std::string targetOutputReal = this->ConvertToFastbuildPath(
    gt->GetFullPath(config, cmStateEnums::RuntimeBinaryArtifact,
                    /*realname=*/true));
  std::string targetOutputImplib = this->ConvertToFastbuildPath(
    gt->GetFullPath(config, cmStateEnums::ImportLibraryArtifact));

  if (config != fileConfig) {
    if (targetOutput ==
        this->ConvertToFastbuildPath(gt->GetFullPath(fileConfig))) {
      return;
    }
    if (targetOutputReal ==
        this->ConvertToFastbuildPath(
          gt->GetFullPath(fileConfig, cmStateEnums::RuntimeBinaryArtifact,
                          /*realname=*/true))) {
      return;
    }
    if (!gt->GetFullName(config, cmStateEnums::ImportLibraryArtifact)
           .empty() &&
        !gt->GetFullName(fileConfig, cmStateEnums::ImportLibraryArtifact)
           .empty() &&
        targetOutputImplib ==
          this->ConvertToFastbuildPath(gt->GetFullPath(
            fileConfig, cmStateEnums::ImportLibraryArtifact))) {
      return;
    }
  }

  auto const tgtNames = this->TargetNames(config);
  if (gt->IsAppBundleOnApple()) {
    // Create the app bundle
    std::string outpath = gt->GetDirectory(config);
    this->OSXBundleGenerator->CreateAppBundle(tgtNames.Output, outpath,
                                              config);

    // Calculate the output path
    targetOutput = cmStrCat(outpath, '/', tgtNames.Output);
    targetOutput = this->ConvertToFastbuildPath(targetOutput);
    targetOutputReal = cmStrCat(outpath, '/', tgtNames.Real);
    targetOutputReal = this->ConvertToFastbuildPath(targetOutputReal);
  } else if (gt->IsFrameworkOnApple()) {
    // Create the library framework.

    cmOSXBundleGenerator::SkipParts bundleSkipParts;
    if (globalGen->GetName() == "Fastbuild Multi-Config") {
      const auto postFix = this->GeneratorTarget->GetFilePostfix(config);
      // Skip creating Info.plist when there are multiple configurations, and
      // the current configuration has a postfix. The non-postfix configuration
      // Info.plist can be used by all the other configurations.
      if (!postFix.empty()) {
        bundleSkipParts.infoPlist = true;
      }
    }

    this->OSXBundleGenerator->CreateFramework(
      tgtNames.Output, gt->GetDirectory(config), config, bundleSkipParts);
  } else if (gt->IsCFBundleOnApple()) {
    // Create the core foundation bundle.
    this->OSXBundleGenerator->CreateCFBundle(tgtNames.Output,
                                             gt->GetDirectory(config), config);
  }

  // Write comments.
  cmGlobalFastbuildGenerator::WriteDivider(this->GetImplFileStream(fileConfig));
  const cmStateEnums::TargetType targetType = gt->GetType();
//this->GetImplFileStream(fileConfig)
  //<< "// NINJA Link build statements for " << cmState::GetTargetTypeName(targetType)
  //<< " target " << this->GetTargetName() << "\n\n";

  cmFastbuildBuild linkBuild(this->LanguageLinkerRule(config));
  cmFastbuildVars& vars = linkBuild.Variables;

  // Compute the comment.
  linkBuild.Comment =
    cmStrCat("Link the ", this->GetVisibleTypeName(), ' ', targetOutputReal);

  // Compute outputs.
  linkBuild.Outputs.push_back(targetOutputReal);
  if (firstForConfig) {
    globalGen->GetByproductsForCleanTarget(config).push_back(targetOutputReal);
  }

  if (this->TargetLinkLanguage(config) == "Swift") {
    vars["SWIFT_LIBRARY_NAME"] = [this, config]() -> std::string {
      cmGeneratorTarget::Names targetNames =
        this->GetGeneratorTarget()->GetLibraryNames(config);
      return targetNames.Base;
    }();

    vars["SWIFT_MODULE_NAME"] = [gt]() -> std::string {
      if (cmProp name = gt->GetProperty("Swift_MODULE_NAME")) {
        return *name;
      }
      return gt->GetName();
    }();

    vars["SWIFT_MODULE"] = [this](const std::string& module) -> std::string {
      std::string directory =
        this->GetLocalGenerator()->GetCurrentBinaryDirectory();
      if (cmProp prop = this->GetGeneratorTarget()->GetProperty(
            "Swift_MODULE_DIRECTORY")) {
        directory = *prop;
      }

      std::string name = module + ".swiftmodule";
      if (cmProp prop =
            this->GetGeneratorTarget()->GetProperty("Swift_MODULE")) {
        name = *prop;
      }

      return this->GetLocalGenerator()->ConvertToOutputFormat(
        this->ConvertToFastbuildPath(directory + "/" + name),
        cmOutputConverter::SHELL);
    }(vars["SWIFT_MODULE_NAME"]);

    const std::string map = cmStrCat(gt->GetSupportDirectory(), '/', config,
                                     '/', "output-file-map.json");
    vars["SWIFT_OUTPUT_FILE_MAP"] =
      this->GetLocalGenerator()->ConvertToOutputFormat(
        this->ConvertToFastbuildPath(map), cmOutputConverter::SHELL);

    vars["SWIFT_SOURCES"] = [this, config]() -> std::string {
      std::vector<cmSourceFile const*> sources;
      std::stringstream oss;

      this->GetGeneratorTarget()->GetObjectSources(sources, config);
      cmLocalGenerator const* LocalGen = this->GetLocalGenerator();
      for (const auto& source : sources) {
        oss << " "
            << LocalGen->ConvertToOutputFormat(
                 this->ConvertToFastbuildPath(this->GetSourceFilePath(source)),
                 cmOutputConverter::SHELL);
      }
      return oss.str();
    }();

    // Since we do not perform object builds, compute the
    // defines/flags/includes here so that they can be passed along
    // appropriately.
    vars["DEFINES"] = this->GetDefines("Swift", config);
    vars["FLAGS"] = this->GetFlags("Swift", config);
    vars["INCLUDES"] = this->GetIncludes("Swift", config);
  }

  // Compute specific libraries to link with.
  if (this->TargetLinkLanguage(config) == "Swift") {
    std::vector<cmSourceFile const*> sources;
    gt->GetObjectSources(sources, config);
    for (const auto& source : sources) {
      linkBuild.Outputs.push_back(
        this->ConvertToFastbuildPath(this->GetObjectFilePath(source, config)));
      linkBuild.ExplicitDeps.push_back(
        this->ConvertToFastbuildPath(this->GetSourceFilePath(source)));
    }
    linkBuild.Outputs.push_back(vars["SWIFT_MODULE"]);
  } else {
    linkBuild.ExplicitDeps = this->GetObjects(config);
  }

  std::vector<std::string> extraISPCObjects =
    this->GetGeneratorTarget()->GetGeneratedISPCObjects(config);
  std::transform(extraISPCObjects.begin(), extraISPCObjects.end(),
                 std::back_inserter(linkBuild.ExplicitDeps),
                 this->MapToFastbuildPath());

  linkBuild.ImplicitDeps =
    this->ComputeLinkDeps(this->TargetLinkLanguage(config), config);

  if (!this->DeviceLinkObject.empty()) {
    linkBuild.ExplicitDeps.push_back(this->DeviceLinkObject);
  }

  std::string frameworkPath;
  std::string linkPath;

  std::string createRule =
    gt->GetCreateRuleVariable(this->TargetLinkLanguage(config), config);
  bool useWatcomQuote = mf->IsOn(createRule + "_USE_WATCOM_QUOTE");
  cmLocalFastbuildGenerator& localGen = *this->GetLocalGenerator();

  vars["TARGET_FILE"] =
    localGen.ConvertToOutputFormat(targetOutputReal, cmOutputConverter::SHELL);

  std::unique_ptr<cmLinkLineComputer> linkLineComputer =
    globalGen->CreateLinkLineComputer(
      this->GetLocalGenerator(),
      this->GetLocalGenerator()->GetStateSnapshot().GetDirectory());
  linkLineComputer->SetUseWatcomQuote(useWatcomQuote);
  linkLineComputer->SetUseFastbuildMulti(globalGen->IsMultiConfig());

  localGen.GetTargetFlags(linkLineComputer.get(), config,
                          vars["LINK_LIBRARIES"], vars["FLAGS"],
                          vars["LINK_FLAGS"], frameworkPath, linkPath, gt);

  // Add OS X version flags, if any.
  if (this->GeneratorTarget->GetType() == cmStateEnums::SHARED_LIBRARY ||
      this->GeneratorTarget->GetType() == cmStateEnums::MODULE_LIBRARY) {
    this->AppendOSXVerFlag(vars["LINK_FLAGS"],
                           this->TargetLinkLanguage(config), "COMPATIBILITY",
                           true);
    this->AppendOSXVerFlag(vars["LINK_FLAGS"],
                           this->TargetLinkLanguage(config), "CURRENT", false);
  }

  this->addPoolFastbuildVariable("JOB_POOL_LINK", gt, vars);

  this->AddModuleDefinitionFlag(linkLineComputer.get(), vars["LINK_FLAGS"],
                                config);
  vars["LINK_FLAGS"] = globalGen->EncodeLiteral(vars["LINK_FLAGS"]);

  vars["MANIFESTS"] = this->GetManifests(config);
  vars["AIX_EXPORTS"] = this->GetAIXExports(config);

  vars["LINK_PATH"] = frameworkPath + linkPath;
  std::string lwyuFlags;
  if (gt->GetPropertyAsBool("LINK_WHAT_YOU_USE")) {
    lwyuFlags = " -Wl,--no-as-needed";
  }

  // Compute architecture specific link flags.  Yes, these go into a different
  // variable for executables, probably due to a mistake made when duplicating
  // code between the Makefile executable and library generators.
  if (targetType == cmStateEnums::EXECUTABLE) {
    std::string t = vars["FLAGS"];
    localGen.AddArchitectureFlags(t, gt, this->TargetLinkLanguage(config),
                                  config);
    t += lwyuFlags;
    vars["FLAGS"] = t;
  } else {
    std::string t = vars["ARCH_FLAGS"];
    localGen.AddArchitectureFlags(t, gt, this->TargetLinkLanguage(config),
                                  config);
    vars["ARCH_FLAGS"] = t;
    t.clear();
    t += lwyuFlags;
    localGen.AddLanguageFlagsForLinking(
      t, gt, this->TargetLinkLanguage(config), config);
    vars["LANGUAGE_COMPILE_FLAGS"] = t;
  }
  if (gt->HasSOName(config)) {
    vars["SONAME_FLAG"] = mf->GetSONameFlag(this->TargetLinkLanguage(config));
    vars["SONAME"] = tgtNames.SharedObject;
    if (targetType == cmStateEnums::SHARED_LIBRARY) {
      std::string install_dir = gt->GetInstallNameDirForBuildTree(config);
      if (!install_dir.empty()) {
        vars["INSTALLNAME_DIR"] = localGen.ConvertToOutputFormat(
          install_dir, cmOutputConverter::SHELL);
      }
    }
  }

  cmFastbuildDeps byproducts;

  if (!tgtNames.ImportLibrary.empty()) {
    const std::string impLibPath = localGen.ConvertToOutputFormat(
      targetOutputImplib, cmOutputConverter::SHELL);
    vars["TARGET_IMPLIB"] = impLibPath;
    this->EnsureParentDirectoryExists(impLibPath);
    if (gt->HasImportLibrary(config)) {
      byproducts.push_back(targetOutputImplib);
      if (firstForConfig) {
        globalGen->GetByproductsForCleanTarget(config).push_back(
          targetOutputImplib);
      }
    }
  }

  if (!this->SetMsvcTargetPdbVariable(vars, config)) {
    // It is common to place debug symbols at a specific place,
    // so we need a plain target name in the rule available.
    std::string prefix;
    std::string base;
    std::string suffix;
    gt->GetFullNameComponents(prefix, base, suffix, config);
    std::string dbg_suffix = ".dbg";
    // TODO: Where to document?
    if (cmProp d = mf->GetDefinition("CMAKE_DEBUG_SYMBOL_SUFFIX")) {
      dbg_suffix = *d;
    }
    vars["TARGET_PDB"] = base + suffix + dbg_suffix;
  }

  const std::string objPath =
    cmStrCat(gt->GetSupportDirectory(), globalGen->ConfigDirectory(config));
  vars["OBJECT_DIR"] = this->GetLocalGenerator()->ConvertToOutputFormat(
    this->ConvertToFastbuildPath(objPath), cmOutputConverter::SHELL);
  this->EnsureDirectoryExists(objPath);

  std::string& linkLibraries = vars["LINK_LIBRARIES"];
  std::string& link_path = vars["LINK_PATH"];
  if (globalGen->IsGCCOnWindows()) {
    // ar.exe can't handle backslashes in rsp files (implicitly used by gcc)
    std::replace(linkLibraries.begin(), linkLibraries.end(), '\\', '/');
    std::replace(link_path.begin(), link_path.end(), '\\', '/');
  }

  const std::vector<cmCustomCommand>* cmdLists[3] = {
    &gt->GetPreBuildCommands(), &gt->GetPreLinkCommands(),
    &gt->GetPostBuildCommands()
  };

  std::vector<std::string> preLinkCmdLines;
  std::vector<std::string> postBuildCmdLines;

  std::vector<std::string>* cmdLineLists[3] = { &preLinkCmdLines,
                                                &preLinkCmdLines,
                                                &postBuildCmdLines };

  for (unsigned i = 0; i != 3; ++i) {
    for (cmCustomCommand const& cc : *cmdLists[i]) {
      if (config == fileConfig ||
          this->GetLocalGenerator()->HasUniqueByproducts(cc.GetByproducts(),
                                                         cc.GetBacktrace())) {
        cmCustomCommandGenerator ccg(cc, fileConfig, this->GetLocalGenerator(),
                                     true, config);
        localGen.AppendCustomCommandLines(ccg, *cmdLineLists[i]);
        std::vector<std::string> const& ccByproducts = ccg.GetByproducts();
        std::transform(ccByproducts.begin(), ccByproducts.end(),
                       std::back_inserter(byproducts), this->MapToFastbuildPath());
        std::transform(
          ccByproducts.begin(), ccByproducts.end(),
          std::back_inserter(globalGen->GetByproductsForCleanTarget()),
          this->MapToFastbuildPath());
      }
    }
  }

  // maybe create .def file from list of objects
  cmGeneratorTarget::ModuleDefinitionInfo const* mdi =
    gt->GetModuleDefinitionInfo(config);
  if (mdi && mdi->DefFileGenerated) {
    std::string cmakeCommand =
      this->GetLocalGenerator()->ConvertToOutputFormat(
        cmSystemTools::GetCMakeCommand(), cmOutputConverter::SHELL);
    std::string cmd =
      cmStrCat(cmakeCommand, " -E __create_def ",
               this->GetLocalGenerator()->ConvertToOutputFormat(
                 mdi->DefFile, cmOutputConverter::SHELL),
               ' ');
    std::string obj_list_file = mdi->DefFile + ".objs";
    cmd += this->GetLocalGenerator()->ConvertToOutputFormat(
      obj_list_file, cmOutputConverter::SHELL);

    cmProp nm_executable = this->GetMakefile()->GetDefinition("CMAKE_NM");
    if (cmNonempty(nm_executable)) {
      cmd += " --nm=";
      cmd += this->LocalCommonGenerator->ConvertToOutputFormat(
        *nm_executable, cmOutputConverter::SHELL);
    }
    preLinkCmdLines.push_back(std::move(cmd));

    // create a list of obj files for the -E __create_def to read
    cmGeneratedFileStream fout(obj_list_file);

    if (mdi->WindowsExportAllSymbols) {
      cmFastbuildDeps objs = this->GetObjects(config);
      for (std::string const& obj : objs) {
        if (cmHasLiteralSuffix(obj, ".obj")) {
          fout << obj << "\n";
        }
      }
    }

    for (cmSourceFile const* src : mdi->Sources) {
      fout << src->GetFullPath() << "\n";
    }
  }
  // If we have any PRE_LINK commands, we need to go back to CMAKE_BINARY_DIR
  // for the link commands.
  if (!preLinkCmdLines.empty()) {
    const std::string homeOutDir = localGen.ConvertToOutputFormat(
      localGen.GetBinaryDirectory(), cmOutputConverter::SHELL);
    preLinkCmdLines.push_back("cd " + homeOutDir);
  }

  vars["PRE_LINK"] = localGen.BuildCommandLine(
    preLinkCmdLines, config, fileConfig, "pre-link", this->GeneratorTarget);
  std::string postBuildCmdLine =
    localGen.BuildCommandLine(postBuildCmdLines, config, fileConfig,
                              "post-build", this->GeneratorTarget);

  cmFastbuildVars symlinkVars;
  bool const symlinkNeeded =
    (targetOutput != targetOutputReal && !gt->IsFrameworkOnApple());
  if (!symlinkNeeded) {
    vars["POST_BUILD"] = postBuildCmdLine;
  } else {
    vars["POST_BUILD"] = cmGlobalFastbuildGenerator::SHELL_NOOP;
    symlinkVars["POST_BUILD"] = postBuildCmdLine;
  }

  std::string cmakeVarLang =
    cmStrCat("CMAKE_", this->TargetLinkLanguage(config));

  // build response file name
  std::string cmakeLinkVar = cmakeVarLang + "_RESPONSE_FILE_LINK_FLAG";

  cmProp flag = this->GetMakefile()->GetDefinition(cmakeLinkVar);

  bool const lang_supports_response =
    !(this->TargetLinkLanguage(config) == "RC" ||
      (this->TargetLinkLanguage(config) == "CUDA" && !flag));
  int commandLineLengthLimit = -1;
  if (!lang_supports_response || !this->ForceResponseFile()) {
    commandLineLengthLimit =
      static_cast<int>(cmSystemTools::CalculateCommandLineLengthLimit()) -
      globalGen->GetRuleCmdLength(linkBuild.Rule);
  }

  linkBuild.RspFile = this->ConvertToFastbuildPath(
    cmStrCat("CMakeFiles/", gt->GetName(),
             globalGen->IsMultiConfig() ? cmStrCat('.', config) : "", ".rsp"));

  // Gather order-only dependencies.
  this->GetLocalGenerator()->AppendTargetDepends(
    gt, linkBuild.OrderOnlyDeps, config, fileConfig, DependOnTargetArtifactFB);

  // Add order-only dependencies on versioning symlinks of shared libs we link.
  if (!this->GeneratorTarget->IsDLLPlatform()) {
    if (cmComputeLinkInformation* cli =
          this->GeneratorTarget->GetLinkInformation(config)) {
      for (auto const& item : cli->GetItems()) {
        if (item.Target &&
            item.Target->GetType() == cmStateEnums::SHARED_LIBRARY &&
            !item.Target->IsFrameworkOnApple()) {
          std::string const& lib =
            this->ConvertToFastbuildPath(item.Target->GetFullPath(config));
          if (std::find(linkBuild.ImplicitDeps.begin(),
                        linkBuild.ImplicitDeps.end(),
                        lib) == linkBuild.ImplicitDeps.end()) {
            linkBuild.OrderOnlyDeps.emplace_back(lib);
          }
        }
      }
    }
  }

  // Ninja should restat after linking if and only if there are byproducts.
  vars["RESTAT"] = byproducts.empty() ? "" : "1";

  for (std::string const& o : byproducts) {
    globalGen->SeenCustomCommandOutput(o);
    linkBuild.Outputs.push_back(o);
  }

  // Write the build statement for this target.
  bool usedResponseFile = false;
  globalGen->WriteBuild(this->GetImplFileStream(fileConfig), linkBuild,
                        commandLineLengthLimit, &usedResponseFile);
  this->WriteLinkRule(usedResponseFile, config);

  if (symlinkNeeded) {
    if (targetType == cmStateEnums::EXECUTABLE) {
      cmFastbuildBuild build("CMAKE_SYMLINK_EXECUTABLE");
      build.Comment = "Create executable symlink " + targetOutput;
      build.Outputs.push_back(targetOutput);
      if (firstForConfig) {
        globalGen->GetByproductsForCleanTarget(config).push_back(targetOutput);
      }
      build.ExplicitDeps.push_back(targetOutputReal);
      build.Variables = std::move(symlinkVars);
      globalGen->WriteBuild(this->GetImplFileStream(fileConfig), build);
    } else {
      cmFastbuildBuild build("CMAKE_SYMLINK_LIBRARY");
      build.Comment = "Create library symlink " + targetOutput;

      std::string const soName = this->ConvertToFastbuildPath(
        this->GetTargetFilePath(tgtNames.SharedObject, config));
      // If one link has to be created.
      if (targetOutputReal == soName || targetOutput == soName) {
        symlinkVars["SONAME"] =
          this->GetLocalGenerator()->ConvertToOutputFormat(
            soName, cmOutputConverter::SHELL);
      } else {
        symlinkVars["SONAME"].clear();
        build.Outputs.push_back(soName);
        if (firstForConfig) {
          globalGen->GetByproductsForCleanTarget(config).push_back(soName);
        }
      }
      build.Outputs.push_back(targetOutput);
      if (firstForConfig) {
        globalGen->GetByproductsForCleanTarget(config).push_back(targetOutput);
      }
      build.ExplicitDeps.push_back(targetOutputReal);
      build.Variables = std::move(symlinkVars);

      globalGen->WriteBuild(this->GetImplFileStream(fileConfig), build);
    }
  }

  // Add aliases for the file name and the target name.
  globalGen->AddTargetAlias(tgtNames.Output, gt, config);
  globalGen->AddTargetAlias(this->GetTargetName(), gt, config);
}

void cmFastbuildNormalTargetGenerator::WriteObjectLibStatement(
  const std::string& config)
{
  // Write a phony output that depends on all object files.
  {
    cmFastbuildBuild build("phony");
    build.Comment = "Object library " + this->GetTargetName();
    this->GetLocalGenerator()->AppendTargetOutputs(this->GetGeneratorTarget(),
                                                   build.Outputs, config);
    this->GetLocalGenerator()->AppendTargetOutputs(
      this->GetGeneratorTarget(),
      this->GetGlobalGenerator()->GetByproductsForCleanTarget(config), config);
    build.ExplicitDeps = this->GetObjects(config);
    this->GetGlobalGenerator()->WriteBuild(this->GetCommonFileStream(), build);
  }

  // Add aliases for the target name.
  this->GetGlobalGenerator()->AddTargetAlias(
    this->GetTargetName(), this->GetGeneratorTarget(), config);
}

cmGeneratorTarget::Names cmFastbuildNormalTargetGenerator::TargetNames(
  const std::string& config) const
{
  if (this->GeneratorTarget->GetType() == cmStateEnums::EXECUTABLE) {
    return this->GeneratorTarget->GetExecutableNames(config);
  }
  return this->GeneratorTarget->GetLibraryNames(config);
}

std::string cmFastbuildNormalTargetGenerator::TargetLinkLanguage(
  const std::string& config) const
{
  return this->GeneratorTarget->GetLinkerLanguage(config);
}
