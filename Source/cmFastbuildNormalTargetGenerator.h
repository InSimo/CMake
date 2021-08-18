/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <string>
#include <vector>

#include "cmGeneratorTarget.h"
#include "cmFastbuildTargetGenerator.h"

class cmFastbuildNormalTargetGenerator : public cmFastbuildTargetGenerator
{
public:
  cmFastbuildNormalTargetGenerator(cmGeneratorTarget* target);
  ~cmFastbuildNormalTargetGenerator() override;

  void Generate(const std::string& config) override;

  std::string GetCompilerId(const std::string& config);

  std::vector<std::string> GetNameDepsTargets(const std::string& config);

  static std::string GetNameFile(std::string namePathFile);

  std::vector<std::string> GetFileDeps(std::string config);

  std::vector<std::string> RemoveDuplicateName(std::vector<std::string> listName);

  std::vector<std::string> GetNameTargetLibraries(bool isMultiConfig,
                                                  std::string config);

  std::string GetNameTargetLibrary(std::string namePathFile, bool isMultiConfig, std::string config);

  static std::string GetOutputExtension(
    const std::string& lang);

  static std::string GetPath(
    const std::string& fullPath);

  void WriteTargetFB(const std::string& config);

private:
  std::string LanguageLinkerRule(const std::string& config) const;
  std::string LanguageLinkerDeviceRule(const std::string& config) const;
  std::string LanguageLinkerCudaDeviceRule(const std::string& config) const;
  std::string LanguageLinkerCudaDeviceCompileRule(
    const std::string& config) const;
  std::string LanguageLinkerCudaFatbinaryRule(const std::string& config) const;

  const char* GetVisibleTypeName() const;
  void WriteLanguagesRules(const std::string& config);

  void WriteLinkRule(bool useResponseFile, const std::string& config);
  void WriteDeviceLinkRules(const std::string& config);
  void WriteNvidiaDeviceLinkRule(bool useResponseFile,
                                 const std::string& config);

  void WriteLinkStatement(const std::string& config,
                          const std::string& fileConfig, bool firstForConfig);
  void WriteDeviceLinkStatement(const std::string& config,
                                const std::string& fileConfig,
                                bool firstForConfig);
  void WriteDeviceLinkStatements(const std::string& config,
                                 const std::vector<std::string>& architectures,
                                 const std::string& output);
  void WriteNvidiaDeviceLinkStatement(const std::string& config,
                                      const std::string& fileConfig,
                                      const std::string& outputDir,
                                      const std::string& output);

  void WriteObjectLibStatement(const std::string& config);

  std::vector<std::string> ComputeLinkCmd(const std::string& config);
  std::vector<std::string> ComputeDeviceLinkCmd();

  // Target name info.
  cmGeneratorTarget::Names TargetNames(const std::string& config) const;
  std::string TargetLinkLanguage(const std::string& config) const;
  std::string DeviceLinkObject;

  void WriteCompileFB(const std::string& config);
  void WriteObjectListsFB(const std::string& config);
  void WriteObjectListFB(const std::string& config,
                         const std::string& language,
                         const std::string& under_objectList_name,
                         std::vector<std::string> objectList,
                         const std::string& compilerOptions,
                         const std::string& output_path,
                         const std::string& SourceFileExtension);
  void GetTargetFlagsFB(const std::string& config, std::string& linkLibs,
                        std::string& flags, std::string& linkFlags);
  std::string GetLinkFlagsFB(const std::string& config, const std::string& language, std::string target_name);
  void WriteExecutableFB(const std::string& config);
  void WriteLibraryFB(const std::string& config);
  void WriteDLLFB(const std::string& config);
};
