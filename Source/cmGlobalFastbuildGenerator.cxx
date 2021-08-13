/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#include "cmGlobalFastbuildGenerator.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

#include <cm/iterator>
#include <cm/memory>
#include <cmext/algorithm>
#include <cmext/memory>

#include <cm3p/json/reader.h>
#include <cm3p/json/value.h>
#include <cm3p/json/writer.h>

#include "cmsys/FStream.hxx"

#include "cmDocumentationEntry.h"
#include "cmFortranParser.h"
#include "cmGeneratedFileStream.h"
#include "cmGeneratorExpressionEvaluationFile.h"
#include "cmGeneratorTarget.h"
#include "cmGlobalGenerator.h"
#include "cmLinkLineComputer.h"
#include "cmListFileCache.h"
#include "cmLocalGenerator.h"
#include "cmLocalFastbuildGenerator.h"
#include "cmMakefile.h"
#include "cmMessageType.h"
#include "cmFastbuildLinkLineComputer.h"
#include "cmOutputConverter.h"
#include "cmProperty.h"
#include "cmRange.h"
#include "cmScanDepFormat.h"
#include "cmState.h"
#include "cmStateDirectory.h"
#include "cmStateSnapshot.h"
#include "cmStateTypes.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmTarget.h"
#include "cmTargetDepend.h"
#include "cmVersion.h"
#include "cmake.h"
#include "cmFastbuildNormalTargetGenerator.h"

const char* cmGlobalFastbuildGenerator::FASTBUILD_BUILD_FILE = "fbuild.bff";
const char* cmGlobalFastbuildGenerator::FASTBUILD_RULES_FILE =
  "CMakeFiles/rules.bff";
const char* cmGlobalFastbuildGenerator::INDENT = "  ";
#ifdef _WIN32
std::string const cmGlobalFastbuildGenerator::SHELL_NOOP = "cd .";
#else
std::string const cmGlobalFastbuildGenerator::SHELL_NOOP = ":";
#endif

bool operator==(
  const cmGlobalFastbuildGenerator::ByConfig::TargetDependsClosureKey& lhs,
  const cmGlobalFastbuildGenerator::ByConfig::TargetDependsClosureKey& rhs)
{
  return lhs.Target == rhs.Target && lhs.Config == rhs.Config &&
    lhs.GenexOutput == rhs.GenexOutput;
}

bool operator!=(
  const cmGlobalFastbuildGenerator::ByConfig::TargetDependsClosureKey& lhs,
  const cmGlobalFastbuildGenerator::ByConfig::TargetDependsClosureKey& rhs)
{
  return !(lhs == rhs);
}

bool operator<(
  const cmGlobalFastbuildGenerator::ByConfig::TargetDependsClosureKey& lhs,
  const cmGlobalFastbuildGenerator::ByConfig::TargetDependsClosureKey& rhs)
{
  return lhs.Target < rhs.Target ||
    (lhs.Target == rhs.Target &&
     (lhs.Config < rhs.Config ||
      (lhs.Config == rhs.Config && lhs.GenexOutput < rhs.GenexOutput)));
}

bool operator>(
  const cmGlobalFastbuildGenerator::ByConfig::TargetDependsClosureKey& lhs,
  const cmGlobalFastbuildGenerator::ByConfig::TargetDependsClosureKey& rhs)
{
  return rhs < lhs;
}

bool operator<=(
  const cmGlobalFastbuildGenerator::ByConfig::TargetDependsClosureKey& lhs,
  const cmGlobalFastbuildGenerator::ByConfig::TargetDependsClosureKey& rhs)
{
  return !(lhs > rhs);
}

bool operator>=(
  const cmGlobalFastbuildGenerator::ByConfig::TargetDependsClosureKey& lhs,
  const cmGlobalFastbuildGenerator::ByConfig::TargetDependsClosureKey& rhs)
{
  return rhs <= lhs;
}

void cmGlobalFastbuildGenerator::Indent(std::ostream& os, int count)
{
  for (int i = 0; i < count; ++i) {
    os << cmGlobalFastbuildGenerator::INDENT;
  }
}

void cmGlobalFastbuildGenerator::WriteDivider(std::ostream& os)
{
  os << "// --------------------------------------"
        "-----------------------------------------\n";
}

std::string cmGlobalFastbuildGenerator::Quote(const std::string& str,
                                              const std::string& quotation)
{
  if (str.empty()) return "";
  return cmStrCat(quotation, str, quotation);
}

void cmGlobalFastbuildGenerator::WriteComment(std::ostream& os,
                                              const std::string& comment)
{
  if (comment.empty()) {
    return;
  }
  os << "// NINJA" << comment << "\n";
}

void cmGlobalFastbuildGenerator::WriteCommentFB(std::ostream& os,
                                                const std::string& comment)
{
  if (comment.empty()) {
    return;
  }
  os << this->linePrefix << "// " << comment << "\n";
}

void cmGlobalFastbuildGenerator::WriteIncludeFB(std::ostream& os,
                                                const std::string& filename,
                                                const std::string& comment)
{
  WriteCommentFB(os, comment);
  os << "#include \"" << filename << "\"\n";
}

void cmGlobalFastbuildGenerator::WriteSectionHeader(std::ostream& os,
                                                    const std::string& comment)
{
  if (comment.empty()) {
    return;
  }
  os << "\n";
  WriteDivider(os);
  WriteCommentFB(os, comment);
  WriteDivider(os);
}

void cmGlobalFastbuildGenerator::WritePushScope(std::ostream& os, char begin,
                                                char end)
{
  os << this->linePrefix << begin << "\n";
  this->linePrefix += "\t";
  this->closingScope += end;
}

void cmGlobalFastbuildGenerator::WritePushScopeStruct(std::ostream& os)
{
  WritePushScope(os, '[', ']');
}

void cmGlobalFastbuildGenerator::WritePopScope(std::ostream& os)
{
  assert(!this->linePrefix.empty());
  this->linePrefix.resize(this->linePrefix.size() - 1);

  os << this->linePrefix << this->closingScope[this->closingScope.size() - 1]
     << "\n";

  this->closingScope.resize(this->closingScope.size() - 1);
}

void cmGlobalFastbuildGenerator::WriteVariableFB(std::ostream& os,
                                                 const std::string& key,
                                                 const std::string& value,
                                                 const std::string& operation)
{
  os << this->linePrefix << "." << key << " " << operation << " " << value
     << "\n";
}

void cmGlobalFastbuildGenerator::WriteCommand(std::ostream& os,
                                              const std::string& command,
                                              const std::string& value)
{
  os << this->linePrefix << command;
  if (!value.empty()) {
    os << "(" << value << ")";
  }
  os << "\n";
}

void cmGlobalFastbuildGenerator::WriteArray(
  std::ostream& os, const std::string& key,
  const std::vector<std::string>& values, char begin, char end,
  const std::string& operation)
{
  WriteVariableFB(os, key, "", operation);
  WritePushScope(os, begin, end);
  size_t size = values.size();
  for (size_t index = 0; index < size; ++index) {
    const std::string& value = values[index];
    os << this->linePrefix << value;
    os << '\n';
  }
  WritePopScope(os);
}

void cmGlobalFastbuildGenerator::WriteAliasFB(std::ostream& os,
                                            const std::string& name_alias,
                                            const std::string& targets)
{
  this->WriteCommand(os, "Alias", name_alias);
  this->WritePushScope(os);
  this->WriteVariableFB(os, "Targets", cmStrCat("{ ", targets, " }"));
  this->WritePopScope(os);
}

void cmGlobalFastbuildGenerator::AddFastbuildInfoTarget(cmGeneratorTarget* gt, std::vector<std::string> name_target_deps, const std::string& config)
{
  std::string target_name = cmStrCat(gt->GetName(), config);
  // We add the target whether she is not already in the map
  if (this->MapFastbuildInfoTargets.find(target_name) ==
      this->MapFastbuildInfoTargets.end()) {
    cmFastbuildInfoTarget fbt;
    fbt.is_treated = false;
    fbt.config = config;
    fbt.name_target_deps = name_target_deps;
    fbt.number_untrated_deps =
      GetNumberUntratedDepsTarget(target_name, name_target_deps);
    fbt.gt = gt;
    this->MapFastbuildInfoTargets.insert(std::make_pair(target_name, fbt));
  }
}

int cmGlobalFastbuildGenerator::GetNumberUntratedDepsTarget(std::string target_name, std::vector<std::string> name_target_deps)
{
  int numberDepsTarget = 0;
  for(std::string name_target : name_target_deps){
    if (name_target != target_name) {
      auto it = this->MapFastbuildInfoTargets.find(name_target);
      if (it != this->MapFastbuildInfoTargets.end()) {
        if (!this->MapFastbuildInfoTargets[name_target].is_treated)
          numberDepsTarget += 1;
      } else {
        numberDepsTarget += 1;
      }
    }
  }
  return numberDepsTarget;
}

bool cmGlobalFastbuildGenerator::CanTreatTargetFB(
  cmGeneratorTarget* gt, const std::string& config)
{
  bool canTreat = true;
  std::string target_name = cmStrCat(gt->GetName(), config);
  if(this->MapFastbuildInfoTargets[target_name].number_untrated_deps > 0 || this->MapFastbuildInfoTargets[target_name].is_treated){
    canTreat = false;
  }
  return canTreat;
}

void cmGlobalFastbuildGenerator::TargetTreatedFinish(cmGeneratorTarget* gt, const std::string& config)
{
  std::string target_name = cmStrCat(gt->GetName(), config);
  this->MapFastbuildInfoTargets[target_name].is_treated = true;
  for(auto fit : this->MapFastbuildInfoTargets){
    //this->MapFastbuildInfoTargets[fit.first].number_untrated_deps = GetNumberUntratedDepsTarget(fit.first, fit.second.name_target_deps);
    for (std::string name_target_dep : fit.second.name_target_deps) {
      if (target_name == name_target_dep)
        this->MapFastbuildInfoTargets[fit.first].number_untrated_deps--;
    }

    // If finally the target can be treat, we treat him immediately
    if (this->CanTreatTargetFB(
          fit.second.gt, this->MapFastbuildInfoTargets[fit.first].config)) {
      cmFastbuildNormalTargetGenerator(this->MapFastbuildInfoTargets[fit.first].gt).Generate(this->MapFastbuildInfoTargets[fit.first].config);
    }
  }
}

void cmGlobalFastbuildGenerator::DecrementNbDepsTargetUnexist()
{
  for (auto fit : this->MapFastbuildInfoTargets) {
    if (!fit.second.is_treated) {
      for (std::string name_target_dep : fit.second.name_target_deps) {
        // if the target does not exist in the map,
        // we have considered that this dependency does not need to be processed by cmake
        // and that we do not need in the order of the dependencies
        if (this->MapFastbuildInfoTargets.find(name_target_dep) ==
            this->MapFastbuildInfoTargets.end()) {
          this->MapFastbuildInfoTargets[fit.first].number_untrated_deps--;
        }
      }
    }
  }
}

void cmGlobalFastbuildGenerator::lastChanceToTreatTargets()
{
  this->DecrementNbDepsTargetUnexist();
  for (auto fit : this->MapFastbuildInfoTargets) {
    if (!fit.second.is_treated) {
      if (this->CanTreatTargetFB(fit.second.gt, fit.second.config)) {
        cmFastbuildNormalTargetGenerator(
          this->MapFastbuildInfoTargets[fit.first].gt)
          .Generate(this->MapFastbuildInfoTargets[fit.first].config);
      }
    }
  }
}

void cmGlobalFastbuildGenerator::PrintAllTargetWithNbDeps()
{
  for (auto fit : this->MapFastbuildInfoTargets) {
    if (!fit.second.is_treated) {
      WriteSectionHeader(*this->GetCommonFileStream(),
                         cmStrCat("WARNING : ", fit.first, " : NB DEPS : ",
                                  fit.second.number_untrated_deps,
                                  " : IS TREATED : ", fit.second.is_treated));

      for (auto ntd : fit.second.name_target_deps) {
        WriteSectionHeader(*this->GetCommonFileStream(),
                           cmStrCat("TARGET DEPS : ", ntd));
      }
    }
  }
}

std::ostream& cmGlobalFastbuildGenerator::GetFileStream(const std::string& config, bool isMultiConfig)
{
  if (!isMultiConfig)
    return *this->GetCommonFileStream();
  else
    return *this->GetImplFileStream(config);
}

std::unique_ptr<cmLinkLineComputer>
cmGlobalFastbuildGenerator::CreateLinkLineComputer(
  cmOutputConverter* outputConverter,
  cmStateDirectory const& /* stateDir */) const
{
  return std::unique_ptr<cmLinkLineComputer>(
    cm::make_unique<cmFastbuildLinkLineComputer>(
      outputConverter,
      this->LocalGenerators[0]->GetStateSnapshot().GetDirectory(), this));
}

std::string cmGlobalFastbuildGenerator::EncodeRuleName(std::string const& name)
{
  // Ninja rule names must match "[a-zA-Z0-9_.-]+".  Use ".xx" to encode
  // "." and all invalid characters as hexadecimal.
  std::string encoded;
  for (char i : name) {
    if (isalnum(i) || i == '_' || i == '-') {
      encoded += i;
    } else {
      char buf[16];
      sprintf(buf, ".%02x", static_cast<unsigned int>(i));
      encoded += buf;
    }
  }
  return encoded;
}

std::string cmGlobalFastbuildGenerator::EncodeLiteral(const std::string& lit)
{
  std::string result = lit;
  cmSystemTools::ReplaceString(result, "$", "$$");
  cmSystemTools::ReplaceString(result, "\n", "$\n");
  if (this->IsMultiConfig()) {
    cmSystemTools::ReplaceString(result,
                                 cmStrCat('$', this->GetCMakeCFGIntDir()),
                                 this->GetCMakeCFGIntDir());
  }
  return result;
}

std::string cmGlobalFastbuildGenerator::EncodePath(const std::string& path)
{
  std::string result = path;
#ifdef _WIN32
  if (this->IsGCCOnWindows())
    std::replace(result.begin(), result.end(), '\\', '/');
  else
    std::replace(result.begin(), result.end(), '/', '\\');
#endif
  result = this->EncodeLiteral(result);
  cmSystemTools::ReplaceString(result, " ", "$ ");
  cmSystemTools::ReplaceString(result, ":", "$:");
  return result;
}

void cmGlobalFastbuildGenerator::WriteBuild(std::ostream& os,
                                        cmFastbuildBuild const& build,
                                        int cmdLineLimit,
                                        bool* usedResponseFile)
{
  // Make sure there is a rule.
  if (build.Rule.empty()) {
    cmSystemTools::Error(cmStrCat(
      "No rule for WriteBuild! called with comment: ", build.Comment));
    return;
  }

  // Make sure there is at least one output file.
  if (build.Outputs.empty()) {
    cmSystemTools::Error(cmStrCat(
      "No output files for WriteBuild! called with comment: ", build.Comment));
    return;
  }

  cmGlobalFastbuildGenerator::WriteComment(os, build.Comment);

  // Write output files.
  std::string buildStr("// NINJA build");
  {
    // Write explicit outputs
    for (std::string const& output : build.Outputs) {
      buildStr += cmStrCat(' ', this->EncodePath(output));
      if (this->ComputingUnknownDependencies) {
        this->CombinedBuildOutputs.insert(output);
      }
    }
    // Write implicit outputs
    if (!build.ImplicitOuts.empty()) {
      buildStr += " |";
      for (std::string const& implicitOut : build.ImplicitOuts) {
        buildStr += cmStrCat(' ', this->EncodePath(implicitOut));
      }
    }
    buildStr += ':';

    // Write the rule.
    buildStr += cmStrCat(' ', build.Rule);
  }

  std::string arguments;
  {
    // TODO: Better formatting for when there are multiple input/output files.

    // Write explicit dependencies.
    for (std::string const& explicitDep : build.ExplicitDeps) {
      arguments += cmStrCat(' ', this->EncodePath(explicitDep));
    }

    // Write implicit dependencies.
    if (!build.ImplicitDeps.empty()) {
      arguments += " |";
      for (std::string const& implicitDep : build.ImplicitDeps) {
        arguments += cmStrCat(' ', this->EncodePath(implicitDep));
      }
    }

    // Write order-only dependencies.
    if (!build.OrderOnlyDeps.empty()) {
      arguments += " ||";
      for (std::string const& orderOnlyDep : build.OrderOnlyDeps) {
        arguments += cmStrCat(' ', this->EncodePath(orderOnlyDep));
      }
    }

    arguments += '\n';
  }

  // Write the variables bound to this build statement.
  std::string assignments;
  {
    std::ostringstream variable_assignments;
    for (auto const& variable : build.Variables) {
      cmGlobalFastbuildGenerator::WriteVariable(
        variable_assignments, variable.first, variable.second, "", 1);
    }

    // check if a response file rule should be used
    assignments = variable_assignments.str();
    bool useResponseFile = false;
    if (cmdLineLimit < 0 ||
        (cmdLineLimit > 0 &&
         (arguments.size() + buildStr.size() + assignments.size() + 1000) >
           static_cast<size_t>(cmdLineLimit))) {
      variable_assignments.str(std::string());
      cmGlobalFastbuildGenerator::WriteVariable(variable_assignments, "RSP_FILE",
                                            build.RspFile, "", 1);
      assignments += variable_assignments.str();
      useResponseFile = true;
    }
    if (usedResponseFile) {
      *usedResponseFile = useResponseFile;
    }
  }

  if (build.Variables.count("dyndep") > 0) {
    // The ninja 'cleandead' operation does not account for outputs
    // discovered by 'dyndep' bindings.  Avoid removing them.
    this->DisableCleandead = true;
  }

  os << buildStr << arguments << assignments << "\n";
}

void cmGlobalFastbuildGenerator::AddCustomCommandRule()
{
  cmFastbuildRule rule("CUSTOM_COMMAND");
  rule.Command = "$COMMAND";
  rule.Description = "$DESC";
  rule.Comment = "Rule for running custom commands.";
  this->AddRule(rule);
}

void cmGlobalFastbuildGenerator::WriteCustomCommandBuild(
  const std::string& command, const std::string& description,
  const std::string& comment, const std::string& depfile,
  const std::string& job_pool, bool uses_terminal, bool restat,
  const cmFastbuildDeps& outputs, const std::string& config,
  const cmFastbuildDeps& explicitDeps, const cmFastbuildDeps& orderOnlyDeps)
{
  this->AddCustomCommandRule();

  {
    cmFastbuildBuild build("CUSTOM_COMMAND");
    build.Comment = comment;
    build.Outputs = outputs;
    build.ExplicitDeps = explicitDeps;
    build.OrderOnlyDeps = orderOnlyDeps;

    cmFastbuildVars& vars = build.Variables;
    {
      std::string cmd = command; // NOLINT(*)
#ifdef _WIN32
      if (cmd.empty())
        // TODO Shouldn't an empty command be handled by ninja?
        cmd = "cmd.exe /c";
#endif
      vars["COMMAND"] = std::move(cmd);
    }
    vars["DESC"] = this->EncodeLiteral(description);
    if (restat) {
      vars["restat"] = "1";
    }
    if (uses_terminal && this->SupportsConsolePool()) {
      vars["pool"] = "console";
    } else if (!job_pool.empty()) {
      vars["pool"] = job_pool;
    }
    if (!depfile.empty()) {
      vars["depfile"] = depfile;
    }
    if (config.empty()) {
      this->WriteBuild(*this->GetCommonFileStream(), build);
    } else {
      this->WriteBuild(*this->GetImplFileStream(config), build);
    }
  }

  if (this->ComputingUnknownDependencies) {
    // we need to track every dependency that comes in, since we are trying
    // to find dependencies that are side effects of build commands
    for (std::string const& dep : explicitDeps) {
      this->CombinedCustomCommandExplicitDependencies.insert(dep);
    }
  }
}

void cmGlobalFastbuildGenerator::WriteCustomCommandBuildFB(
  std::vector<std::string> commands, const std::string& description,
  const std::string& comment, const std::string& depfile,
  const std::string& job_pool, bool uses_terminal, bool restat,
  const cmFastbuildDeps& outputs, const std::string& config,
  const cmFastbuildDeps& explicitDeps, const cmFastbuildDeps& orderOnlyDeps,
  const std::string& random_name_file, const std::string& workingDirectory,
  bool excludeFromAll)
{
  std::ostream& os = this->GetFileStream(config, this->IsMultiConfig());
  std::string command = "";
  int i = 0;
  for (auto a : commands) {
    if (i == 0 && commands.size() > 1) {
      i++;
      continue;
    }
    if (i > 1 && i<commands.size())
      command += " && ";
    command += a;
    i++;
  }

  std::string output = ""; 
  for (auto a : outputs) {
    output += a;
    if (!random_name_file.empty()) {
      output += "/";
      output += random_name_file;
    }
  }
  std::string explicitDep = "";
  for (auto a : explicitDeps) {
    explicitDep += " ";
    explicitDep += a;
  }
  std::string orderOnlyDep = "";
  for (auto a : orderOnlyDeps) {
    orderOnlyDep += " ";
    orderOnlyDep += a;
  }
  std::string execUseStdOutAsOutput = "false";
  if (!random_name_file.empty()) {
    execUseStdOutAsOutput = "true";
  }

  std::string arguments = "";
  auto found = command.find(" ");
  if (found != std::string::npos) {
    arguments = command.substr(found);
    command = command.substr(0, found);
  }

  std::string name_exec = cmStrCat(cmFastbuildNormalTargetGenerator::GetNameFile(output), config);
  auto it = name_exec.find("-");
  while (it != std::string::npos) {
    name_exec.replace(it, 1, "_");
    it = name_exec.find("-");
  }

  std::string workingDir = workingDirectory;
  if (workingDirectory.empty())
    workingDir = "./";

  this->WriteSectionHeader(os, comment);
  this->WriteCommand(os, "Exec", this->Quote(name_exec));
  this->WritePushScope(os);
  this->WriteVariableFB(os, "CONFIGURATION", this->Quote(config));
  this->WriteVariableFB(os, "ExecExecutable", this->Quote(command));
  if (!arguments.empty())
    this->WriteVariableFB(os, "ExecArguments", this->Quote(arguments));
  this->WriteVariableFB(os, "ExecOutput", this->Quote(output));
  this->WriteVariableFB(os, "ExecWorkingDir", this->Quote(workingDir));
  this->WriteVariableFB(os, "ExecUseStdOutAsOutput", execUseStdOutAsOutput);
  this->WriteVariableFB(os, "ExecAlways", "true");
  this->WritePopScope(os);
  /*
  this->WriteSectionHeader(os, cmStrCat("COMMAND : ", command));
  this->WriteSectionHeader(os, cmStrCat("DESCRIPTION : ", description));
  this->WriteSectionHeader(os, cmStrCat("COMMENT : ", comment));
  this->WriteSectionHeader(os, cmStrCat("DEPFILE : ", depfile));
  this->WriteSectionHeader(os, cmStrCat("JOB POOL : ", job_pool));
  this->WriteSectionHeader(os, cmStrCat("OUTPUTS : ", output));
  this->WriteSectionHeader(os, cmStrCat("EXPLICIT DEPS : ", explicitDep));
  this->WriteSectionHeader(os, cmStrCat("ORDER ONLY DEPS : ", orderOnlyDep));
  */

  std::string alias_name = cmStrCat(name_exec, "-deps"); 
  this->AddTargetAliasFB(this->Quote(alias_name), this->Quote(name_exec),
                           config, excludeFromAll);
}

void cmGlobalFastbuildGenerator::AddMacOSXContentRule()
{
  cmFastbuildRule rule("COPY_OSX_CONTENT");
  rule.Command = cmStrCat(this->CMakeCmd(), " -E copy $in $out");
  rule.Description = "Copying OS X Content $out";
  rule.Comment = "Rule for copying OS X bundle content file.";
  this->AddRule(rule);
}

void cmGlobalFastbuildGenerator::WriteMacOSXContentBuild(std::string input,
                                                     std::string output,
                                                     const std::string& config)
{
  this->AddMacOSXContentRule();
  {
    cmFastbuildBuild build("COPY_OSX_CONTENT");
    build.Outputs.push_back(std::move(output));
    build.ExplicitDeps.push_back(std::move(input));
    this->WriteBuild(*this->GetImplFileStream(config), build);
  }
}

void cmGlobalFastbuildGenerator::WriteRule(std::ostream& os,
                                       cmFastbuildRule const& rule)
{
  // -- Parameter checks
  // Make sure the rule has a name.
  if (rule.Name.empty()) {
    cmSystemTools::Error(cmStrCat(
      "No name given for WriteRule! called with comment: ", rule.Comment));
    return;
  }

  // Make sure a command is given.
  if (rule.Command.empty()) {
    cmSystemTools::Error(cmStrCat(
      "No command given for WriteRule! called with comment: ", rule.Comment));
    return;
  }

  // Make sure response file content is given
  if (!rule.RspFile.empty() && rule.RspContent.empty()) {
    cmSystemTools::Error(
      cmStrCat("rspfile but no rspfile_content given for WriteRule! "
               "called with comment: ",
               rule.Comment));
    return;
  }

  // -- Write rule
  // Write rule intro
  cmGlobalFastbuildGenerator::WriteComment(os, rule.Comment);
  os << "// NINJA " << "rule " << rule.Name << '\n';

  // Write rule key/value pairs
  auto writeKV = [&os](const char* key, std::string const& value) {
    if (!value.empty()) {
      cmGlobalFastbuildGenerator::Indent(os, 1);
      os << "// NINJA" << key << " = " << value << '\n';
    }
  };

  writeKV("depfile", rule.DepFile);
  writeKV("deps", rule.DepType);
  writeKV("command", rule.Command);
  writeKV("description", rule.Description);
  if (!rule.RspFile.empty()) {
    writeKV("rspfile", rule.RspFile);
    writeKV("rspfile_content", rule.RspContent);
  }
  writeKV("restat", rule.Restat);
  if (rule.Generator) {
    writeKV("generator", "1");
  }

  // Finish rule
  os << '\n';
}

void cmGlobalFastbuildGenerator::WriteVariable(std::ostream& os,
                                           const std::string& name,
                                           const std::string& value,
                                           const std::string& comment,
                                           int indent)
{
  // Make sure we have a name.
  if (name.empty()) {
    cmSystemTools::Error(cmStrCat("// No name given for WriteVariable! called "
                                  "// with comment: ",
                                  comment));
    return;
  }

  // Do not add a variable if the value is empty.
  std::string val = cmTrimWhitespace(value);
  if (val.empty()) {
    return;
  }

  cmGlobalFastbuildGenerator::WriteComment(os, comment);
  cmGlobalFastbuildGenerator::Indent(os, indent);
  os << "// NINJA " << name << " = " << val << "\n";
}

void cmGlobalFastbuildGenerator::WriteInclude(std::ostream& os,
                                          const std::string& filename,
                                          const std::string& comment)
{
  cmGlobalFastbuildGenerator::WriteComment(os, comment);
  os << "// NINJA include " << filename << "\n";
}

void cmGlobalFastbuildGenerator::WriteDefault(std::ostream& os,
                                          const cmFastbuildDeps& targets,
                                          const std::string& comment)
{
  cmGlobalFastbuildGenerator::WriteComment(os, comment);
  os << "// NINJA default";
  for (std::string const& target : targets) {
    os << " " << target;
  }
  os << "\n";
}

cmGlobalFastbuildGenerator::cmGlobalFastbuildGenerator(cmake* cm)
  : cmGlobalCommonGenerator(cm)
{
#ifdef _WIN32
  cm->GetState()->SetWindowsShell(true);
#endif
  this->FindMakeProgramFile = "CMakeFastbuildFindMake.cmake";
}

// Virtual public methods.

std::unique_ptr<cmLocalGenerator> cmGlobalFastbuildGenerator::CreateLocalGenerator(
  cmMakefile* mf)
{
  return std::unique_ptr<cmLocalGenerator>(
    cm::make_unique<cmLocalFastbuildGenerator>(this, mf));
}

codecvt::Encoding cmGlobalFastbuildGenerator::GetMakefileEncoding() const
{
#ifdef _WIN32
  // Ninja on Windows does not support non-ANSI characters.
  // https://github.com/ninja-build/ninja/issues/1195
  return codecvt::ANSI;
#else
  // No encoding conversion needed on other platforms.
  return codecvt::None;
#endif
}

void cmGlobalFastbuildGenerator::GetDocumentation(cmDocumentationEntry& entry)
{
  entry.Name = cmGlobalFastbuildGenerator::GetActualName();
  entry.Brief = "Generates fbuild.bff files.";
}

// Implemented in all cmGlobaleGenerator sub-classes.
// Used in:
//   Source/cmLocalGenerator.cxx
//   Source/cmake.cxx
void cmGlobalFastbuildGenerator::Generate()
{
  // Check minimum Fastbuild version.
  if (cmSystemTools::VersionCompare(cmSystemTools::OP_LESS,
                                    this->FastbuildVersion.c_str(),
                                    RequiredFastbuildVersion().c_str())) {
    std::ostringstream msg;
    msg << "The detected version of Fastbuild (" << this->FastbuildVersion;
    msg << ") is less than the version of Fastbuild required by CMake (";
    msg << cmGlobalFastbuildGenerator::RequiredFastbuildVersion() << ").";
    this->GetCMakeInstance()->IssueMessage(MessageType::FATAL_ERROR,
                                           msg.str());
    return;
  }
  if (!this->InspectConfigTypeVariables()) {
    return;
  }
  if (!this->OpenBuildFileStreams()) {
    return;
  }
  if (!this->OpenRulesFileStream()) {
    return;
  }

  for (auto& it : this->Configs) {
    it.second.TargetDependsClosures.clear();
  }

  this->InitOutputPathPrefix();
  this->TargetAll = this->FastbuildOutputPath("all");
  this->CMakeCacheFile = this->FastbuildOutputPath("CMakeCache.txt");
  this->DisableCleandead = false;
  this->DiagnosedCxxModuleSupport = false;

  this->PolicyCMP0058 =
    this->LocalGenerators[0]->GetMakefile()->GetPolicyStatus(
      cmPolicies::CMP0058);
  this->ComputingUnknownDependencies =
    (this->PolicyCMP0058 == cmPolicies::OLD ||
     this->PolicyCMP0058 == cmPolicies::WARN);

  // For file .bff formatting
  this->linePrefix = "";
  this->closingScope = "";


  // For Fastbuild
  // this->WriteSettings(*this->GetCommonFileStream());
  this->WritePlaceholders(*this->GetCommonFileStream());
  
  this->cmGlobalGenerator::Generate();

  this->lastChanceToTreatTargets();

  this->WriteTargetAliasesFB();
  this->PrintAllTargetWithNbDeps();

  this->WriteAssumedSourceDependencies();
  this->WriteTargetAliases(*this->GetCommonFileStream());
  this->WriteFolderTargets(*this->GetCommonFileStream());
  this->WriteUnknownExplicitDependencies(*this->GetCommonFileStream());
  this->WriteBuiltinTargets(*this->GetCommonFileStream());

  if (cmSystemTools::GetErrorOccuredFlag()) {
    this->RulesFileStream->setstate(std::ios::failbit);
    for (auto const& config : this->Makefiles[0]->GetGeneratorConfigs(
           cmMakefile::IncludeEmptyConfig)) {
      this->GetImplFileStream(config)->setstate(std::ios::failbit);
      this->GetConfigFileStream(config)->setstate(std::ios::failbit);
    }
    this->GetCommonFileStream()->setstate(std::ios::failbit);
  }

  this->CloseCompileCommandsStream();
  this->CloseRulesFileStream();
  this->CloseBuildFileStreams();

  std::srand(std::time(nullptr));
  std::string name = cmStrCat("C:/Users/lgross/Documents/tmp/", rand());
  std::ofstream tempFile(name + ".bff");

  std::ifstream rf;
  rf.open(cmGlobalFastbuildGenerator::FASTBUILD_RULES_FILE);
  std::ifstream bf;
  bf.open(cmGlobalFastbuildGenerator::FASTBUILD_BUILD_FILE);
  std::string line;
  while (std::getline(rf, line)) {
    tempFile << line << std::endl;
  }
  while (std::getline(bf, line)) {
    tempFile << line << std::endl;
  }
  rf.close();
  bf.close();
  tempFile.close();

#ifdef _WIN32
  // Older ninja tools will not be able to update metadata on Windows
  // when we are re-generating inside an existing 'ninja' invocation
  // because the outer tool has the files open for write.
  if (this->FastbuildSupportsMetadataOnRegeneration ||
      !this->GetCMakeInstance()->GetRegenerateDuringBuild())
#endif
  {
    //this->CleanMetaData(); TMP
  }
}

void cmGlobalFastbuildGenerator::WritePlaceholders(std::ostream& os)
{
  WriteSectionHeader(os, "Helper variables");
  WriteVariableFB(os, "FB_INPUT_1_PLACEHOLDER", Quote("\"%1\""));
  WriteVariableFB(os, "FB_INPUT_2_PLACEHOLDER", Quote("\"%2\""));
}

void cmGlobalFastbuildGenerator::WriteSettings(std::ostream& os)
{
  WriteSectionHeader(os, "Settings");
  os << "Settings\n";
  WritePushScope(os);
  WritePopScope(os);
}

void cmGlobalFastbuildGenerator::CleanMetaData()
{
  auto run_fastbuild_tool = [this](std::vector<char const*> const& args) {
    std::vector<std::string> command;
    command.push_back(this->FastbuildCommand);
    command.emplace_back("-C");
    command.emplace_back(this->GetCMakeInstance()->GetHomeOutputDirectory());
    command.emplace_back("-t");
    for (auto const& arg : args) {
      command.emplace_back(arg);
    }
    std::string error;
    if (!cmSystemTools::RunSingleCommand(command, nullptr, &error, nullptr,
                                         nullptr,
                                         cmSystemTools::OUTPUT_NONE)) {
      this->GetCMakeInstance()->IssueMessage(MessageType::FATAL_ERROR,
                                             cmStrCat("Running\n '",
                                                      cmJoin(command, "' '"),
                                                      "'\n"
                                                      "failed with:\n ",
                                                      error));
      cmSystemTools::SetFatalErrorOccured();
    }
  };

  // Can the tools below expect 'build.ninja' to be loadable?
  bool const expectBuildManifest =
    !this->IsMultiConfig() && this->OutputPathPrefix.empty();

  // Skip some ninja tools if they need 'build.ninja' but it is missing.
  bool const missingBuildManifest = expectBuildManifest &&
    this->FastbuildSupportsUnconditionalRecompactTool &&
    !cmSystemTools::FileExists("fbuild.bff");

  // The `recompact` tool loads the manifest. As above, we don't have a single
  // `build.ninja` to load for this in Ninja-Multi. This may be relaxed in the
  // future pending further investigation into how Ninja works upstream
  // (ninja#1721).
  if (this->FastbuildSupportsUnconditionalRecompactTool &&
      !this->GetCMakeInstance()->GetRegenerateDuringBuild() &&
      expectBuildManifest && !missingBuildManifest) {
    run_fastbuild_tool({ "recompact" });
  }
  if (this->FastbuildSupportsRestatTool && this->OutputPathPrefix.empty()) {
    // XXX(ninja): We only list `build.ninja` entry files here because CMake
    // *always* rewrites these files on a reconfigure. If CMake ever gets
    // smarter about this, all CMake-time created/edited files listed as
    // outputs for the reconfigure build statement will need to be listed here.
    cmFastbuildDeps outputs;
    this->AddRebuildManifestOutputs(outputs);
    std::vector<const char*> args;
    args.reserve(outputs.size() + 1);
    args.push_back("restat");
    for (auto const& output : outputs) {
      args.push_back(output.c_str());
    }
    run_fastbuild_tool(args);
  }
}

bool cmGlobalFastbuildGenerator::FindMakeProgram(cmMakefile* mf)
{
  if (!this->cmGlobalGenerator::FindMakeProgram(mf)) {
    return false;
  }
  if (cmProp fastbuildCommand = mf->GetDefinition("CMAKE_MAKE_PROGRAM")) {
    this->FastbuildCommand = *fastbuildCommand;
    std::vector<std::string> command;
    command.push_back(this->FastbuildCommand);
    command.emplace_back("-version");
    std::string version;
    std::string error;
    if (!cmSystemTools::RunSingleCommand(command, &version, &error, nullptr,
                                         nullptr,
                                         cmSystemTools::OUTPUT_NONE)) {
      mf->IssueMessage(MessageType::FATAL_ERROR,
                       cmStrCat("Running\n '", cmJoin(command, "' '"),
                                "'\n"
                                "failed with:\n ",
                                error));
      cmSystemTools::SetFatalErrorOccured();
      return false;
    }
    this->FastbuildVersion = cmTrimWhitespace(version);
    this->CheckFastbuildFeatures();
  }
  return true;
}

void cmGlobalFastbuildGenerator::CheckFastbuildFeatures()
{
  this->FastbuildSupportsConsolePool = !cmSystemTools::VersionCompare(
    cmSystemTools::OP_LESS, this->FastbuildVersion.c_str(),
    RequiredFastbuildVersionForConsolePool().c_str());
  this->FastbuildSupportsImplicitOuts = !cmSystemTools::VersionCompare(
    cmSystemTools::OP_LESS, this->FastbuildVersion.c_str(),
    cmGlobalFastbuildGenerator::RequiredFastbuildVersionForImplicitOuts().c_str());
  this->FastbuildSupportsManifestRestat = !cmSystemTools::VersionCompare(
    cmSystemTools::OP_LESS, this->FastbuildVersion.c_str(),
    RequiredFastbuildVersionForManifestRestat().c_str());
  this->FastbuildSupportsMultilineDepfile = !cmSystemTools::VersionCompare(
    cmSystemTools::OP_LESS, this->FastbuildVersion.c_str(),
    RequiredFastbuildVersionForMultilineDepfile().c_str());
  this->FastbuildSupportsDyndeps = !cmSystemTools::VersionCompare(
    cmSystemTools::OP_LESS, this->FastbuildVersion.c_str(),
    RequiredFastbuildVersionForDyndeps().c_str());
  if (!this->FastbuildSupportsDyndeps) {
    // The ninja version number is not new enough to have upstream support.
    // Our ninja branch adds ".dyndep-#" to its version number,
    // where '#' is a feature-specific version number.  Extract it.
    static std::string const k_DYNDEP_ = ".dyndep-";
    std::string::size_type pos = this->FastbuildVersion.find(k_DYNDEP_);
    if (pos != std::string::npos) {
      const char* fv = &this->FastbuildVersion[pos + k_DYNDEP_.size()];
      unsigned long dyndep = 0;
      cmStrToULong(fv, &dyndep);
      if (dyndep == 1) {
        this->FastbuildSupportsDyndeps = true;
      }
    }
  }
  this->FastbuildSupportsUnconditionalRecompactTool =
    !cmSystemTools::VersionCompare(
      cmSystemTools::OP_LESS, this->FastbuildVersion.c_str(),
      RequiredFastbuildVersionForUnconditionalRecompactTool().c_str());
  this->FastbuildSupportsRestatTool = !cmSystemTools::VersionCompare(
    cmSystemTools::OP_LESS, this->FastbuildVersion.c_str(),
    RequiredFastbuildVersionForRestatTool().c_str());
  this->FastbuildSupportsMultipleOutputs = !cmSystemTools::VersionCompare(
    cmSystemTools::OP_LESS, this->FastbuildVersion.c_str(),
    RequiredFastbuildVersionForMultipleOutputs().c_str());
  this->FastbuildSupportsMetadataOnRegeneration = !cmSystemTools::VersionCompare(
    cmSystemTools::OP_LESS, this->FastbuildVersion.c_str(),
    RequiredFastbuildVersionForMetadataOnRegeneration().c_str());
}

bool cmGlobalFastbuildGenerator::CheckLanguages(
  std::vector<std::string> const& languages, cmMakefile* mf) const
{
  if (cm::contains(languages, "Fortran")) {
    return this->CheckFortran(mf);
  }
  if (cm::contains(languages, "ISPC")) {
    return this->CheckISPC(mf);
  }
  if (cm::contains(languages, "Swift")) {
    const std::string architectures =
      mf->GetSafeDefinition("CMAKE_OSX_ARCHITECTURES");
    if (architectures.find_first_of(';') != std::string::npos) {
      mf->IssueMessage(MessageType::FATAL_ERROR,
                       "multiple values for CMAKE_OSX_ARCHITECTURES not "
                       "supported with Swift");
      cmSystemTools::SetFatalErrorOccured();
      return false;
    }
  }
  return true;
}

bool cmGlobalFastbuildGenerator::CheckCxxModuleSupport()
{
  bool const diagnose = !this->DiagnosedCxxModuleSupport &&
    !this->CMakeInstance->GetIsInTryCompile();
  if (diagnose) {
    this->DiagnosedCxxModuleSupport = true;
    this->GetCMakeInstance()->IssueMessage(
      MessageType::AUTHOR_WARNING,
      "C++20 modules support via CMAKE_EXPERIMENTAL_CXX_MODULE_DYNDEP "
      "is experimental.  It is meant only for compiler developers to try.");
  }
  if (this->FastbuildSupportsDyndeps) {
    return true;
  }
  if (diagnose) {
    std::ostringstream e;
    /* clang-format off */
    e <<
      "The Fastbuild generator does not support C++20 modules "
      "using Fastbuild version \n"
      "  " << this->FastbuildVersion << "\n"
      "due to lack of required features.  "
      "Fastbuild " << RequiredFastbuildVersionForDyndeps() << " or higher is required."
      ;
    /* clang-format on */
    this->GetCMakeInstance()->IssueMessage(MessageType::FATAL_ERROR, e.str());
    cmSystemTools::SetFatalErrorOccured();
  }
  return false;
}

bool cmGlobalFastbuildGenerator::CheckFortran(cmMakefile* mf) const
{
  if (this->FastbuildSupportsDyndeps) {
    return true;
  }

  std::ostringstream e;
  /* clang-format off */
  e <<
    "The Fastbuild generator does not support Fortran using Fastbuild version\n"
    "  " << this->FastbuildVersion << "\n"
    "due to lack of required features.  "
    "Fastbuild " << RequiredFastbuildVersionForDyndeps() << " or higher is required."
    ;
  /* clang-format on */
  mf->IssueMessage(MessageType::FATAL_ERROR, e.str());
  cmSystemTools::SetFatalErrorOccured();
  return false;
}

bool cmGlobalFastbuildGenerator::CheckISPC(cmMakefile* mf) const
{
  if (this->FastbuildSupportsMultipleOutputs) {
    return true;
  }

  std::ostringstream e;
  /* clang-format off */
  e <<
    "The Fastbuild generator does not support ISPC using Fastbuild version\n"
    "  " << this->FastbuildVersion << "\n"
    "due to lack of required features.  "
    "Fastbuild " << RequiredFastbuildVersionForMultipleOutputs() <<
    " or higher is required."
    ;
  /* clang-format on */
  mf->IssueMessage(MessageType::FATAL_ERROR, e.str());
  cmSystemTools::SetFatalErrorOccured();
  return false;
}

void cmGlobalFastbuildGenerator::EnableLanguage(
  std::vector<std::string> const& langs, cmMakefile* mf, bool optional)
{
  if (this->IsMultiConfig()) {
    if (!mf->GetDefinition("CMAKE_CONFIGURATION_TYPES")) {
      mf->AddCacheDefinition(
        "CMAKE_CONFIGURATION_TYPES", "Debug;Release;RelWithDebInfo",
        "Semicolon separated list of supported configuration types, only "
        "supports Debug, Release, MinSizeRel, and RelWithDebInfo, anything "
        "else will be ignored",
        cmStateEnums::STRING);
    }
  }

  this->cmGlobalGenerator::EnableLanguage(langs, mf, optional);
  for (std::string const& l : langs) {
    if (l == "NONE") {
      continue;
    }
    this->ResolveLanguageCompiler(l, mf, optional);
  }
#ifdef _WIN32
  const bool clangGnuMode =
    ((mf->GetSafeDefinition("CMAKE_C_COMPILER_ID") == "Clang") &&
     (mf->GetSafeDefinition("CMAKE_C_COMPILER_FRONTEND_VARIANT") == "GNU")) ||
    ((mf->GetSafeDefinition("CMAKE_CXX_COMPILER_ID") == "Clang") &&
     (mf->GetSafeDefinition("CMAKE_CXX_COMPILER_FRONTEND_VARIANT") == "GNU"));

  if (clangGnuMode ||
      ((mf->GetSafeDefinition("CMAKE_C_SIMULATE_ID") != "MSVC") &&
       (mf->GetSafeDefinition("CMAKE_CXX_SIMULATE_ID") != "MSVC") &&
       (mf->IsOn("CMAKE_COMPILER_IS_MINGW") ||
        (mf->GetSafeDefinition("CMAKE_C_COMPILER_ID") == "GNU") ||
        (mf->GetSafeDefinition("CMAKE_CXX_COMPILER_ID") == "GNU") ||
        (mf->GetSafeDefinition("CMAKE_C_COMPILER_ID") == "Clang") ||
        (mf->GetSafeDefinition("CMAKE_CXX_COMPILER_ID") == "Clang") ||
        (mf->GetSafeDefinition("CMAKE_C_COMPILER_ID") == "QCC") ||
        (mf->GetSafeDefinition("CMAKE_CXX_COMPILER_ID") == "QCC")))) {
    this->UsingGCCOnWindows = true;
  }
#endif
}

// Implemented by:
//   cmGlobalUnixMakefileGenerator3
//   cmGlobalGhsMultiGenerator
//   cmGlobalVisualStudio10Generator
//   cmGlobalVisualStudio7Generator
//   cmGlobalXCodeGenerator
// Called by:
//   cmGlobalGenerator::Build()
std::vector<cmGlobalGenerator::GeneratedMakeCommand>
cmGlobalFastbuildGenerator::GenerateBuildCommand(
  const std::string& makeProgram, const std::string& /*projectName*/,
  const std::string& /*projectDir*/,
  std::vector<std::string> const& targetNames, const std::string& config,
  bool /*fast*/, int jobs, bool verbose,
  std::vector<std::string> const& makeOptions)
{
  GeneratedMakeCommand makeCommand;
  makeCommand.Add(this->SelectMakeProgram(makeProgram));

  if (verbose) {
    makeCommand.Add("-v");
  }

  if ((jobs != cmake::NO_BUILD_PARALLEL_LEVEL) &&
      (jobs != cmake::DEFAULT_BUILD_PARALLEL_LEVEL)) {
    makeCommand.Add("-j", std::to_string(jobs));
  }

  this->AppendFastbuildFileArgument(makeCommand, config);

  makeCommand.Add(makeOptions.begin(), makeOptions.end());
  for (const auto& tname : targetNames) {
    if (!tname.empty()) {
      makeCommand.Add(tname);
    }
  }
  return { std::move(makeCommand) };
}

// Non-virtual public methods.

void cmGlobalFastbuildGenerator::AddRule(cmFastbuildRule const& rule)
{
  // Do not add the same rule twice.
  if (!this->Rules.insert(rule.Name).second) {
    return;
  }
  // Store command length
  this->RuleCmdLength[rule.Name] = static_cast<int>(rule.Command.size());
  // Write rule
  cmGlobalFastbuildGenerator::WriteRule(*this->RulesFileStream, rule);
}

bool cmGlobalFastbuildGenerator::HasRule(const std::string& name)
{
  return (this->Rules.find(name) != this->Rules.end());
}

// Private virtual overrides

std::string cmGlobalFastbuildGenerator::GetEditCacheCommand() const
{
  // Ninja by design does not run interactive tools in the terminal,
  // so our only choice is cmake-gui.
  return cmSystemTools::GetCMakeGUICommand();
}

void cmGlobalFastbuildGenerator::ComputeTargetObjectDirectory(
  cmGeneratorTarget* gt) const
{
  // Compute full path to object file directory for this target.
  std::string dir = cmStrCat(gt->LocalGenerator->GetCurrentBinaryDirectory(),
                             '/', gt->LocalGenerator->GetTargetDirectory(gt),
                             '/', this->GetCMakeCFGIntDir(), '/');
  gt->ObjectDirectory = dir;
}

// Private methods

bool cmGlobalFastbuildGenerator::OpenBuildFileStreams()
{
  if (!this->OpenFileStream(this->BuildFileStream,
                            cmGlobalFastbuildGenerator::FASTBUILD_BUILD_FILE)) {
    return false;
  }

  // Write a comment about this file.
  *this->BuildFileStream
    << "// This file contains all the build statements describing the\n"
    << "// compilation DAG.\n\n";

  return true;
}

bool cmGlobalFastbuildGenerator::OpenFileStream(
  std::unique_ptr<cmGeneratedFileStream>& stream, const std::string& name)
{
  // Get a stream where to generate things.
  if (!stream) {
    // Compute Ninja's build file path.
    std::string path =
      cmStrCat(this->GetCMakeInstance()->GetHomeOutputDirectory(), '/', name);
    stream = cm::make_unique<cmGeneratedFileStream>(
      path, false, this->GetMakefileEncoding());
    if (!(*stream)) {
      // An error message is generated by the constructor if it cannot
      // open the file.
      return false;
    }

    // Write the do not edit header.
    this->WriteDisclaimer(*stream);
  }

  return true;
}

cm::optional<std::set<std::string>> cmGlobalFastbuildGenerator::ListSubsetWithAll(
  const std::set<std::string>& all, const std::set<std::string>& defaults,
  const std::vector<std::string>& items)
{
  std::set<std::string> result;

  for (auto const& item : items) {
    if (item == "all") {
      if (items.size() == 1) {
        result = defaults;
      } else {
        return cm::nullopt;
      }
    } else if (all.count(item)) {
      result.insert(item);
    } else {
      return cm::nullopt;
    }
  }

  return cm::make_optional(result);
}

void cmGlobalFastbuildGenerator::CloseBuildFileStreams()
{
  if (this->BuildFileStream) {
    this->BuildFileStream.reset();
  } else {
    cmSystemTools::Error("Build file stream was not open.");
  }
}

bool cmGlobalFastbuildGenerator::OpenRulesFileStream()
{
  if (!this->OpenFileStream(this->RulesFileStream,
                            cmGlobalFastbuildGenerator::FASTBUILD_RULES_FILE)) {
    return false;
  }

  // Write comment about this file.
  /* clang-format off */
  *this->RulesFileStream
    << "// This file contains all the rules used to get the outputs files\n"
    << "// built from the input files.\n"
    << "// It is included in the main '" << FASTBUILD_BUILD_FILE << "'.\n\n"
    ;
  /* clang-format on */
  return true;
}

void cmGlobalFastbuildGenerator::CloseRulesFileStream()
{
  if (this->RulesFileStream) {
    this->RulesFileStream.reset();
  } else {
    cmSystemTools::Error("Rules file stream was not open.");
  }
}

static void EnsureTrailingSlash(std::string& path)
{
  if (path.empty()) {
    return;
  }
  std::string::value_type last = path.back();
#ifdef _WIN32
  if (last != '\\') {
    path += '\\';
  }
#else
  if (last != '/') {
    path += '/';
  }
#endif
}

std::string const& cmGlobalFastbuildGenerator::ConvertToFastbuildPath(
  const std::string& path) const
{
  auto const f = this->ConvertToFastbuildPathCache.find(path);
  if (f != this->ConvertToFastbuildPathCache.end()) {
    return f->second;
  }

  const auto& ng =
    cm::static_reference_cast<cmLocalFastbuildGenerator>(this->LocalGenerators[0]);
  std::string const& bin_dir = ng.GetState()->GetBinaryDirectory();
  std::string convPath = ng.MaybeConvertToRelativePath(bin_dir, path);
  convPath = this->FastbuildOutputPath(convPath);
#ifdef _WIN32
  std::replace(convPath.begin(), convPath.end(), '/', '\\');
#endif
  return this->ConvertToFastbuildPathCache.emplace(path, std::move(convPath))
    .first->second;
}

void cmGlobalFastbuildGenerator::AddAdditionalCleanFile(std::string fileName,
                                                    const std::string& config)
{
  this->Configs[config].AdditionalCleanFiles.emplace(std::move(fileName));
}

void cmGlobalFastbuildGenerator::AddCXXCompileCommand(
  const std::string& commandLine, const std::string& sourceFile)
{
  // Compute Ninja's build file path.
  std::string buildFileDir =
    this->GetCMakeInstance()->GetHomeOutputDirectory();
  if (!this->CompileCommandsStream) {
    std::string buildFilePath =
      cmStrCat(buildFileDir, "/compile_commands.json");
    if (this->ComputingUnknownDependencies) {
      this->CombinedBuildOutputs.insert(
        this->FastbuildOutputPath("compile_commands.json"));
    }

    // Get a stream where to generate things.
    this->CompileCommandsStream =
      cm::make_unique<cmGeneratedFileStream>(buildFilePath);
    *this->CompileCommandsStream << "[\n";
  } else {
    *this->CompileCommandsStream << ",\n";
  }

  std::string sourceFileName = sourceFile;
  if (!cmSystemTools::FileIsFullPath(sourceFileName)) {
    sourceFileName = cmSystemTools::CollapseFullPath(
      sourceFileName, this->GetCMakeInstance()->GetHomeOutputDirectory());
  }

  /* clang-format off */
  *this->CompileCommandsStream << "{\n"
     << R"(  "directory": ")"
     << cmGlobalGenerator::EscapeJSON(buildFileDir) << "\",\n"
     << R"(  "command": ")"
     << cmGlobalGenerator::EscapeJSON(commandLine) << "\",\n"
     << R"(  "file": ")"
     << cmGlobalGenerator::EscapeJSON(sourceFileName) << "\"\n"
     << "}";
  /* clang-format on */
}

void cmGlobalFastbuildGenerator::CloseCompileCommandsStream()
{
  if (this->CompileCommandsStream) {
    *this->CompileCommandsStream << "\n]";
    this->CompileCommandsStream.reset();
  }
}

void cmGlobalFastbuildGenerator::WriteDisclaimer(std::ostream& os) const
{
  os << "// CMAKE generated file: DO NOT EDIT!\n"
     << "// Generated by \"" << this->GetName() << "\""
     << " Generator, CMake Version " << cmVersion::GetMajorVersion() << "."
     << cmVersion::GetMinorVersion() << "\n\n";
}

void cmGlobalFastbuildGenerator::WriteAssumedSourceDependencies()
{
  for (auto const& asd : this->AssumedSourceDependencies) {
    cmFastbuildDeps orderOnlyDeps;
    std::copy(asd.second.begin(), asd.second.end(),
              std::back_inserter(orderOnlyDeps));
    this->WriteCustomCommandBuild(
      /*command=*/"", /*description=*/"",
      "Assume dependencies for generated source file.",
      /*depfile*/ "", /*job_pool*/ "",
      /*uses_terminal*/ false,
      /*restat*/ true, cmFastbuildDeps(1, asd.first), "", cmFastbuildDeps(),
      orderOnlyDeps);
  }
}

std::string cmGlobalFastbuildGenerator::OrderDependsTargetForTarget(
  cmGeneratorTarget const* target, const std::string& /*config*/) const
{
  return cmStrCat(" cmake_object_order_depends_target_", target->GetName());
}

void cmGlobalFastbuildGenerator::AppendTargetOutputs(
  cmGeneratorTarget const* target, cmFastbuildDeps& outputs,
  const std::string& config, cmFastbuildTargetDepends depends) const
{
  // for frameworks, we want the real name, not smple name
  // frameworks always appear versioned, and the build.ninja
  // will always attempt to manage symbolic links instead
  // of letting cmOSXBundleGenerator do it.
  bool realname = target->IsFrameworkOnApple();

  switch (target->GetType()) {
    case cmStateEnums::SHARED_LIBRARY:
    case cmStateEnums::STATIC_LIBRARY:
    case cmStateEnums::MODULE_LIBRARY: {
      if (depends == DependOnTargetOrderingFB) {
        outputs.push_back(this->OrderDependsTargetForTarget(target, config));
        break;
      }
    }
    // FALLTHROUGH
    case cmStateEnums::EXECUTABLE: {
      outputs.push_back(this->ConvertToFastbuildPath(target->GetFullPath(
        config, cmStateEnums::RuntimeBinaryArtifact, realname)));
      break;
    }
    case cmStateEnums::OBJECT_LIBRARY: {
      if (depends == DependOnTargetOrderingFB) {
        outputs.push_back(this->OrderDependsTargetForTarget(target, config));
        break;
      }
    }
    // FALLTHROUGH
    case cmStateEnums::GLOBAL_TARGET:
    case cmStateEnums::INTERFACE_LIBRARY:
    case cmStateEnums::UTILITY: {
      std::string path =
        cmStrCat(target->GetLocalGenerator()->GetCurrentBinaryDirectory(), '/',
                 target->GetName());
      std::string output = this->ConvertToFastbuildPath(path);
      if (target->Target->IsPerConfig()) {
        output = this->BuildAlias(output, config);
      }
      outputs.push_back(output);
      break;
    }

    case cmStateEnums::UNKNOWN_LIBRARY:
      break;
  }
}

void cmGlobalFastbuildGenerator::AppendTargetDepends(
  cmGeneratorTarget const* target, cmFastbuildDeps& outputs,
  const std::string& config, const std::string& fileConfig,
  cmFastbuildTargetDepends depends)
{
  if (target->GetType() == cmStateEnums::GLOBAL_TARGET) {
    // These depend only on other CMake-provided targets, e.g. "all".
    for (BT<std::pair<std::string, bool>> const& util :
         target->GetUtilities()) {
      std::string d =
        cmStrCat(target->GetLocalGenerator()->GetCurrentBinaryDirectory(), '/',
                 util.Value.first);
      outputs.push_back(this->BuildAlias(this->ConvertToFastbuildPath(d), config));
    }
  } else {
    cmFastbuildDeps outs;

    auto computeISPCOuputs = [](cmGlobalFastbuildGenerator* gg,
                                cmGeneratorTarget const* depTarget,
                                cmFastbuildDeps& outputDeps,
                                const std::string& targetConfig) {
      if (depTarget->CanCompileSources()) {
        auto headers = depTarget->GetGeneratedISPCHeaders(targetConfig);
        if (!headers.empty()) {
          std::transform(headers.begin(), headers.end(), headers.begin(),
                         gg->MapToFastbuildPath());
          outputDeps.insert(outputDeps.end(), headers.begin(), headers.end());
        }
        auto objs = depTarget->GetGeneratedISPCObjects(targetConfig);
        if (!objs.empty()) {
          std::transform(objs.begin(), objs.end(), objs.begin(),
                         gg->MapToFastbuildPath());
          outputDeps.insert(outputDeps.end(), objs.begin(), objs.end());
        }
      }
    };

    for (cmTargetDepend const& targetDep :
         this->GetTargetDirectDepends(target)) {
      if (!targetDep->IsInBuildSystem()) {
        continue;
      }
      if (targetDep.IsCross()) {
        this->AppendTargetOutputs(targetDep, outs, fileConfig, depends);
        computeISPCOuputs(this, targetDep, outs, fileConfig);
      } else {
        this->AppendTargetOutputs(targetDep, outs, config, depends);
        computeISPCOuputs(this, targetDep, outs, config);
      }
    }
    std::sort(outs.begin(), outs.end());
    cm::append(outputs, outs);
  }
}

void cmGlobalFastbuildGenerator::AppendTargetDependsClosure(
  cmGeneratorTarget const* target, cmFastbuildDeps& outputs,
  const std::string& config, const std::string& fileConfig, bool genexOutput)
{
  cmFastbuildOuts outs;
  this->AppendTargetDependsClosure(target, outs, config, fileConfig,
                                   genexOutput, true);
  cm::append(outputs, outs);
}

void cmGlobalFastbuildGenerator::AppendTargetDependsClosure(
  cmGeneratorTarget const* target, cmFastbuildOuts& outputs,
  const std::string& config, const std::string& fileConfig, bool genexOutput,
  bool omit_self)
{

  // try to locate the target in the cache
  ByConfig::TargetDependsClosureKey key{
    target,
    config,
    genexOutput,
  };
  auto find = this->Configs[fileConfig].TargetDependsClosures.lower_bound(key);

  if (find == this->Configs[fileConfig].TargetDependsClosures.end() ||
      find->first != key) {
    // We now calculate the closure outputs by inspecting the dependent
    // targets recursively.
    // For that we have to distinguish between a local result set that is only
    // relevant for filling the cache entries properly isolated and a global
    // result set that is relevant for the result of the top level call to
    // AppendTargetDependsClosure.
    cmFastbuildOuts this_outs; // this will be the new cache entry

    for (auto const& dep_target : this->GetTargetDirectDepends(target)) {
      if (!dep_target->IsInBuildSystem()) {
        continue;
      }

      if (!this->IsSingleConfigUtility(target) &&
          !this->IsSingleConfigUtility(dep_target) &&
          this->EnableCrossConfigBuild() && !dep_target.IsCross() &&
          !genexOutput) {
        continue;
      }

      if (dep_target.IsCross()) {
        this->AppendTargetDependsClosure(dep_target, this_outs, fileConfig,
                                         fileConfig, genexOutput, false);
      } else {
        this->AppendTargetDependsClosure(dep_target, this_outs, config,
                                         fileConfig, genexOutput, false);
      }
    }
    find = this->Configs[fileConfig].TargetDependsClosures.emplace_hint(
      find, key, std::move(this_outs));
  }

  // now fill the outputs of the final result from the newly generated cache
  // entry
  outputs.insert(find->second.begin(), find->second.end());

  // finally generate the outputs of the target itself, if applicable
  cmFastbuildDeps outs;
  if (!omit_self) {
    this->AppendTargetOutputs(target, outs, config, DependOnTargetArtifactFB);
  }
  outputs.insert(outs.begin(), outs.end());
}

void cmGlobalFastbuildGenerator::AddTargetAliasFB(const std::string& alias,
                                                  std::string listDeps,
                                                  const std::string& config, bool excludeFromAll)
{
  if (this->TargetAliasesFB.find(alias) == this->TargetAliasesFB.end()) {
    TargetAliasFB ta;
    ta.Config = config;
    ta.ListDeps = listDeps;
    ta.ExcludeFromAll = excludeFromAll;
    this->TargetAliasesFB.insert(std::make_pair(alias, ta));
    this->TargetAliasesOrderedFB.push_back(alias);
  }
}

void cmGlobalFastbuildGenerator::WriteTargetAliasesFB()
{
  if (!IsMultiConfig()) {
    std::ostream& os = *this->GetCommonFileStream();
    std::string listDepsAll = "";
    std::string listDepsOther = "";
    this->WriteSectionHeader(os, "Target aliases");
    for (std::string name_alias : this->TargetAliasesOrderedFB) {
      auto ta = this->TargetAliasesFB[name_alias];

      WriteAliasFB(os, name_alias, ta.ListDeps);
      if (ta.ExcludeFromAll) {
        listDepsOther += name_alias;
      } else {
        listDepsAll += name_alias;
      }
    }

    //Write Alias all
    WriteAliasFB(os, Quote("all"), listDepsAll);
    // Write Alias other
    if (!listDepsOther.empty()) {
      WriteAliasFB(os, Quote("other"), listDepsOther);
    }
  } else {
    std::map<std::string, std::string> configsAll;
    std::map<std::string, std::string> configsOther;
    configsAll["Debug"] = "";
    configsAll["RelWithDebInfo"] = "";
    configsAll["Release"] = "";
    configsOther["Debug"] = "";
    configsOther["RelWithDebInfo"] = "";
    configsOther["Release"] = "";

    for (auto config : configsAll) {
      std::ostream& os = *this->GetImplFileStream(config.first);
      this->WriteSectionHeader(os,
                               cmStrCat("Target aliases : ", config.first));
      for (std::string name_alias : this->TargetAliasesOrderedFB) {
        auto ta = this->TargetAliasesFB[name_alias];
        if (ta.Config != config.first) {
          continue;
        }
        WriteAliasFB(os, name_alias, ta.ListDeps);
        if (ta.ExcludeFromAll) {
          configsOther[ta.Config] += name_alias;
        } else {
          configsAll[ta.Config] += name_alias;
        }
      }
    }
    // Write Alias all
    for (auto config : configsAll) {
      WriteAliasFB(*this->GetImplFileStream(config.first),
                   Quote("all"), config.second);
    }
    // Write Alias other
    for (auto config : configsOther) {
      if (!config.second.empty()) {
        WriteAliasFB(*this->GetImplFileStream(config.first), Quote("other"),
                     config.second);
      }
    }
  }
}

void cmGlobalFastbuildGenerator::InitFastbuildNormalTargetGenerators()
{
  // Voir si on peut initialiser FastbuildNormalTargetGenerators avec le nom de toute les targets du projet et mettre le cmFastbuildTargetStatus NOT_TREAT
}

void cmGlobalFastbuildGenerator::AddTargetAlias(const std::string& alias,
                                            cmGeneratorTarget* target,
                                            const std::string& config)
{
  std::string outputPath = this->FastbuildOutputPath(alias);
  std::string buildAlias = this->BuildAlias(outputPath, config);
  cmFastbuildDeps outputs;
  if (config != "all") {
    this->AppendTargetOutputs(target, outputs, config, DependOnTargetArtifactFB);
  }
  // Mark the target's outputs as ambiguous to ensure that no other target
  // uses the output as an alias.
  for (std::string const& output : outputs) {
    this->TargetAliases[output].GeneratorTarget = nullptr;
    this->DefaultTargetAliases[output].GeneratorTarget = nullptr;
    for (const std::string& config2 :
         this->Makefiles.front()->GetGeneratorConfigs(
           cmMakefile::IncludeEmptyConfig)) {
      this->Configs[config2].TargetAliases[output].GeneratorTarget = nullptr;
    }
  }

  // Insert the alias into the map.  If the alias was already present in the
  // map and referred to another target, mark it as ambiguous.
  TargetAlias ta;
  ta.GeneratorTarget = target;
  ta.Config = config;

  auto newAliasGlobal =
    this->TargetAliases.insert(std::make_pair(buildAlias, ta));
  if (newAliasGlobal.second &&
      newAliasGlobal.first->second.GeneratorTarget != target) {
    newAliasGlobal.first->second.GeneratorTarget = nullptr;
  }

  auto newAliasConfig =
    this->Configs[config].TargetAliases.insert(std::make_pair(outputPath, ta));
  if (newAliasConfig.second &&
      newAliasConfig.first->second.GeneratorTarget != target) {
    newAliasConfig.first->second.GeneratorTarget = nullptr;
  }
  if (this->DefaultConfigs.count(config)) {
    auto newAliasDefaultGlobal =
      this->DefaultTargetAliases.insert(std::make_pair(outputPath, ta));
    if (newAliasDefaultGlobal.second &&
        newAliasDefaultGlobal.first->second.GeneratorTarget != target) {
      newAliasDefaultGlobal.first->second.GeneratorTarget = nullptr;
    }
  }
}

void cmGlobalFastbuildGenerator::WriteTargetAliases(std::ostream& os)
{
  cmGlobalFastbuildGenerator::WriteDivider(os);
  os << "// NINJA Target aliases.\n\n";
  cmFastbuildBuild build("phony");
  build.Outputs.emplace_back();
  for (auto const& ta : this->TargetAliases) {
    // Don't write ambiguous aliases.
    if (!ta.second.GeneratorTarget) {
      continue;
    }

    // Don't write alias if there is a already a custom command with
    // matching output
    if (this->HasCustomCommandOutput(ta.first)) {
      continue;
    }
    build.Outputs.front() = ta.first;
    build.ExplicitDeps.clear();
    if (ta.second.Config == "all") {
      for (auto const& config : this->CrossConfigs) {
        this->AppendTargetOutputs(ta.second.GeneratorTarget,
                                  build.ExplicitDeps, config,
                                  DependOnTargetArtifactFB);
      }
    } else {
      this->AppendTargetOutputs(ta.second.GeneratorTarget, build.ExplicitDeps,
                                ta.second.Config, DependOnTargetArtifactFB);
    }
    this->WriteBuild(this->EnableCrossConfigBuild() &&
                         (ta.second.Config == "all" ||
                          this->CrossConfigs.count(ta.second.Config))
                       ? os
                       : *this->GetImplFileStream(ta.second.Config),
                     build);
  }

  if (this->IsMultiConfig()) {
    for (auto const& config : this->Makefiles.front()->GetGeneratorConfigs(
           cmMakefile::IncludeEmptyConfig)) {
      for (auto const& ta : this->Configs[config].TargetAliases) {
        // Don't write ambiguous aliases.
        if (!ta.second.GeneratorTarget) {
          continue;
        }

        // Don't write alias if there is a already a custom command with
        // matching output
        if (this->HasCustomCommandOutput(ta.first)) {
          continue;
        }

        build.Outputs.front() = ta.first;
        build.ExplicitDeps.clear();
        this->AppendTargetOutputs(ta.second.GeneratorTarget,
                                  build.ExplicitDeps, config,
                                  DependOnTargetArtifactFB);
        this->WriteBuild(*this->GetConfigFileStream(config), build);
      }
    }

    if (!this->DefaultConfigs.empty()) {
      for (auto const& ta : this->DefaultTargetAliases) {
        // Don't write ambiguous aliases.
        if (!ta.second.GeneratorTarget) {
          continue;
        }

        // Don't write alias if there is a already a custom command with
        // matching output
        if (this->HasCustomCommandOutput(ta.first)) {
          continue;
        }

        build.Outputs.front() = ta.first;
        build.ExplicitDeps.clear();
        for (auto const& config : this->DefaultConfigs) {
          this->AppendTargetOutputs(ta.second.GeneratorTarget,
                                    build.ExplicitDeps, config,
                                    DependOnTargetArtifactFB);
        }
        this->WriteBuild(*this->GetDefaultFileStream(), build);
      }
    }
  }
}

void cmGlobalFastbuildGenerator::WriteFolderTargets(std::ostream& os)
{
  cmGlobalFastbuildGenerator::WriteDivider(os);
  os << "// NINJA Folder targets.\n\n";

  std::map<std::string, DirectoryTarget> dirTargets =
    this->ComputeDirectoryTargets();

  for (auto const& it : dirTargets) {
    cmFastbuildBuild build("phony");
    cmGlobalFastbuildGenerator::WriteDivider(os);
    std::string const& currentBinaryDir = it.first;
    DirectoryTarget const& dt = it.second;
    std::vector<std::string> configs =
      dt.LG->GetMakefile()->GetGeneratorConfigs(
        cmMakefile::IncludeEmptyConfig);

    // Setup target
    cmFastbuildDeps configDeps;
    build.Comment = cmStrCat("// NINJA Folder: ", currentBinaryDir);
    build.Outputs.emplace_back();
    std::string const buildDirAllTarget =
      this->ConvertToFastbuildPath(cmStrCat(currentBinaryDir, "/all"));
    for (auto const& config : configs) {
      build.ExplicitDeps.clear();
      build.Outputs.front() = this->BuildAlias(buildDirAllTarget, config);
      configDeps.emplace_back(build.Outputs.front());
      for (DirectoryTarget::Target const& t : dt.Targets) {
        if (!this->IsExcludedFromAllInConfig(t, config)) {
          this->AppendTargetOutputs(t.GT, build.ExplicitDeps, config,
                                    DependOnTargetArtifactFB);
        }
      }
      for (DirectoryTarget::Dir const& d : dt.Children) {
        if (!d.ExcludeFromAll) {
          build.ExplicitDeps.emplace_back(this->BuildAlias(
            this->ConvertToFastbuildPath(cmStrCat(d.Path, "/all")), config));
        }
      }
      // Write target
      this->WriteBuild(this->EnableCrossConfigBuild() &&
                           this->CrossConfigs.count(config)
                         ? os
                         : *this->GetImplFileStream(config),
                       build);
    }

    // Add shortcut target
    if (this->IsMultiConfig()) {
      for (auto const& config : configs) {
        build.ExplicitDeps = { this->BuildAlias(buildDirAllTarget, config) };
        build.Outputs.front() = buildDirAllTarget;
        this->WriteBuild(*this->GetConfigFileStream(config), build);
      }

      if (!this->DefaultFileConfig.empty()) {
        build.ExplicitDeps.clear();
        for (auto const& config : this->DefaultConfigs) {
          build.ExplicitDeps.push_back(
            this->BuildAlias(buildDirAllTarget, config));
        }
        build.Outputs.front() = buildDirAllTarget;
        this->WriteBuild(*this->GetDefaultFileStream(), build);
      }
    }

    // Add target for all configs
    if (this->EnableCrossConfigBuild()) {
      build.ExplicitDeps.clear();
      for (auto const& config : this->CrossConfigs) {
        build.ExplicitDeps.push_back(
          this->BuildAlias(buildDirAllTarget, config));
      }
      build.Outputs.front() = this->BuildAlias(buildDirAllTarget, "all");
      this->WriteBuild(os, build);
    }
  }
}

void cmGlobalFastbuildGenerator::WriteUnknownExplicitDependencies(std::ostream& os)
{
  if (!this->ComputingUnknownDependencies) {
    return;
  }

  // We need to collect the set of known build outputs.
  // Start with those generated by WriteBuild calls.
  // No other method needs this so we can take ownership
  // of the set locally and throw it out when we are done.
  std::set<std::string> knownDependencies;
  knownDependencies.swap(this->CombinedBuildOutputs);

  // now write out the unknown explicit dependencies.

  // union the configured files, evaluations files and the
  // CombinedBuildOutputs,
  // and then difference with CombinedExplicitDependencies to find the explicit
  // dependencies that we have no rule for

  cmGlobalFastbuildGenerator::WriteDivider(os);
  /* clang-format off */
  os << "// Unknown Build Time Dependencies.\n"
     << "// Tell Fastbuild that they may appear as side effects of build rules\n"
     << "// otherwise ordered by order-only dependencies.\n\n";
  /* clang-format on */

  // get the list of files that cmake itself has generated as a
  // product of configuration.

  for (const auto& lg : this->LocalGenerators) {
    // get the vector of files created by this makefile and convert them
    // to ninja paths, which are all relative in respect to the build directory
    for (std::string const& file : lg->GetMakefile()->GetOutputFiles()) {
      knownDependencies.insert(this->ConvertToFastbuildPath(file));
    }
    if (!this->GlobalSettingIsOn("CMAKE_SUPPRESS_REGENERATION")) {
      // get list files which are implicit dependencies as well and will be
      // phony for rebuild manifest
      for (std::string const& j : lg->GetMakefile()->GetListFiles()) {
        knownDependencies.insert(this->ConvertToFastbuildPath(j));
      }
    }
    for (const auto& li : lg->GetMakefile()->GetEvaluationFiles()) {
      // get all the files created by generator expressions and convert them
      // to ninja paths
      for (std::string const& evaluationFile : li->GetFiles()) {
        knownDependencies.insert(this->ConvertToFastbuildPath(evaluationFile));
      }
    }
  }
  knownDependencies.insert(this->CMakeCacheFile);

  for (auto const& ta : this->TargetAliases) {
    knownDependencies.insert(this->ConvertToFastbuildPath(ta.first));
  }

  // remove all source files we know will exist.
  for (auto const& i : this->AssumedSourceDependencies) {
    knownDependencies.insert(this->ConvertToFastbuildPath(i.first));
  }

  // now we difference with CombinedCustomCommandExplicitDependencies to find
  // the list of items we know nothing about.
  // We have encoded all the paths in CombinedCustomCommandExplicitDependencies
  // and knownDependencies so no matter if unix or windows paths they
  // should all match now.

  std::vector<std::string> unknownExplicitDepends;
  this->CombinedCustomCommandExplicitDependencies.erase(this->TargetAll);

  std::set_difference(this->CombinedCustomCommandExplicitDependencies.begin(),
                      this->CombinedCustomCommandExplicitDependencies.end(),
                      knownDependencies.begin(), knownDependencies.end(),
                      std::back_inserter(unknownExplicitDepends));

  std::vector<std::string> warnExplicitDepends;
  if (!unknownExplicitDepends.empty()) {
    cmake* cmk = this->GetCMakeInstance();
    std::string const& buildRoot = cmk->GetHomeOutputDirectory();
    bool const inSource = (buildRoot == cmk->GetHomeDirectory());
    bool const warn = (!inSource && (this->PolicyCMP0058 == cmPolicies::WARN));
    cmFastbuildBuild build("phony");
    build.Outputs.emplace_back("");
    for (std::string const& ued : unknownExplicitDepends) {
      // verify the file is in the build directory
      std::string const absDepPath =
        cmSystemTools::CollapseFullPath(ued, buildRoot);
      if (cmSystemTools::IsSubDirectory(absDepPath, buildRoot)) {
        // Generate phony build statement
        build.Outputs[0] = ued;
        this->WriteBuild(os, build);
        // Add to warning on demand
        if (warn && warnExplicitDepends.size() < 10) {
          warnExplicitDepends.push_back(ued);
        }
      }
    }
  }

  if (!warnExplicitDepends.empty()) {
    std::ostringstream w;
    /* clang-format off */
    w << cmPolicies::GetPolicyWarning(cmPolicies::CMP0058) << "\n"
      "This project specifies custom command DEPENDS on files "
      "in the build tree that are not specified as the OUTPUT or "
      "BYPRODUCTS of any add_custom_command or add_custom_target:\n"
      " " << cmJoin(warnExplicitDepends, "\n ") <<
      "\n"
      "For compatibility with versions of CMake that did not have "
      "the BYPRODUCTS option, CMake is generating phony rules for "
      "such files to convince 'fastbuild' to build."
      "\n"
      "Project authors should add the missing BYPRODUCTS or OUTPUT "
      "options to the custom commands that produce these files."
      ;
    /* clang-format on */
    this->GetCMakeInstance()->IssueMessage(MessageType::AUTHOR_WARNING,
                                           w.str());
  }
}

void cmGlobalFastbuildGenerator::WriteBuiltinTargets(std::ostream& os)
{
  // Write headers.
  cmGlobalFastbuildGenerator::WriteDivider(os);
  os << "// NINJA Built-in targets\n\n";

  this->WriteTargetRebuildManifest(os);
  this->WriteTargetClean(os);
  this->WriteTargetHelp(os);

  for (auto const& config : this->Makefiles[0]->GetGeneratorConfigs(
         cmMakefile::IncludeEmptyConfig)) {
    this->WriteTargetDefault(*this->GetConfigFileStream(config));
  }

  if (!this->DefaultFileConfig.empty()) {
    this->WriteTargetDefault(*this->GetDefaultFileStream());
  }
}

void cmGlobalFastbuildGenerator::WriteTargetDefault(std::ostream& os)
{
  if (!this->HasOutputPathPrefix()) {
    cmFastbuildDeps all;
    all.push_back(this->TargetAll);
    cmGlobalFastbuildGenerator::WriteDefault(os, all,
                                         "Make the all target the default.");
  }
}

void cmGlobalFastbuildGenerator::WriteTargetRebuildManifest(std::ostream& os)
{
  if (this->GlobalSettingIsOn("CMAKE_SUPPRESS_REGENERATION")) {
    return;
  }
  const auto& lg = this->LocalGenerators[0];

  {
    cmFastbuildRule rule("RERUN_CMAKE");
    rule.Command =
      cmStrCat(this->CMakeCmd(), " --regenerate-during-build -S",
               lg->ConvertToOutputFormat(lg->GetSourceDirectory(),
                                         cmOutputConverter::SHELL),
               " -B",
               lg->ConvertToOutputFormat(lg->GetBinaryDirectory(),
                                         cmOutputConverter::SHELL));
    rule.Description = "Re-running CMake...";
    rule.Comment = "Rule for re-running cmake.";
    rule.Generator = true;
    WriteRule(*this->RulesFileStream, rule);
  }

  cmFastbuildBuild reBuild("RERUN_CMAKE");
  reBuild.Comment = "Re-run CMake if any of its inputs changed.";
  this->AddRebuildManifestOutputs(reBuild.Outputs);

  for (const auto& localGen : this->LocalGenerators) {
    for (std::string const& fi : localGen->GetMakefile()->GetListFiles()) {
      reBuild.ImplicitDeps.push_back(this->ConvertToFastbuildPath(fi));
    }
  }
  reBuild.ImplicitDeps.push_back(this->CMakeCacheFile);

  // Use 'console' pool to get non buffered output of the CMake re-run call
  // Available since Ninja 1.5
  if (this->SupportsConsolePool()) {
    reBuild.Variables["pool"] = "console";
  }

  cmake* cm = this->GetCMakeInstance();
  if (this->SupportsManifestRestat() && cm->DoWriteGlobVerifyTarget()) {
    {
      cmFastbuildRule rule("VERIFY_GLOBS");
      rule.Command =
        cmStrCat(this->CMakeCmd(), " -P ",
                 lg->ConvertToOutputFormat(cm->GetGlobVerifyScript(),
                                           cmOutputConverter::SHELL));
      rule.Description = "Re-checking globbed directories...";
      rule.Comment = "Rule for re-checking globbed directories.";
      rule.Generator = true;
      this->WriteRule(*this->RulesFileStream, rule);
    }

    cmFastbuildBuild phonyBuild("phony");
    phonyBuild.Comment = "Phony target to force glob verification run.";
    phonyBuild.Outputs.push_back(
      cmStrCat(cm->GetGlobVerifyScript(), "_force"));
    this->WriteBuild(os, phonyBuild);

    reBuild.Variables["restat"] = "1";
    std::string const verifyScriptFile =
      this->FastbuildOutputPath(cm->GetGlobVerifyScript());
    std::string const verifyStampFile =
      this->FastbuildOutputPath(cm->GetGlobVerifyStamp());
    {
      cmFastbuildBuild vgBuild("VERIFY_GLOBS");
      vgBuild.Comment =
        "Re-run CMake to check if globbed directories changed.";
      vgBuild.Outputs.push_back(verifyStampFile);
      vgBuild.ImplicitDeps = phonyBuild.Outputs;
      vgBuild.Variables = reBuild.Variables;
      this->WriteBuild(os, vgBuild);
    }
    reBuild.Variables.erase("restat");
    reBuild.ImplicitDeps.push_back(verifyScriptFile);
    reBuild.ExplicitDeps.push_back(verifyStampFile);
  } else if (!this->SupportsManifestRestat() &&
             cm->DoWriteGlobVerifyTarget()) {
    std::ostringstream msg;
    msg << "The detected version of Fastbuild:\n"
        << "  " << this->FastbuildVersion << "\n"
        << "is less than the version of Fastbuild required by CMake for adding "
           "restat dependencies to the fbuild.bff manifest regeneration "
           "target:\n"
        << "  "
        << cmGlobalFastbuildGenerator::RequiredFastbuildVersionForManifestRestat()
        << "\n";
    msg << "Any pre-check scripts, such as those generated for file(GLOB "
           "CONFIGURE_DEPENDS), will not be run by Fastbuild.";
    this->GetCMakeInstance()->IssueMessage(MessageType::AUTHOR_WARNING,
                                           msg.str());
  }

  std::sort(reBuild.ImplicitDeps.begin(), reBuild.ImplicitDeps.end());
  reBuild.ImplicitDeps.erase(
    std::unique(reBuild.ImplicitDeps.begin(), reBuild.ImplicitDeps.end()),
    reBuild.ImplicitDeps.end());

  this->WriteBuild(os, reBuild);

  {
    cmFastbuildBuild build("phony");
    build.Comment = "A missing CMake input file is not an error.";
    std::set_difference(std::make_move_iterator(reBuild.ImplicitDeps.begin()),
                        std::make_move_iterator(reBuild.ImplicitDeps.end()),
                        this->CustomCommandOutputs.begin(),
                        this->CustomCommandOutputs.end(),
                        std::back_inserter(build.Outputs));
    this->WriteBuild(os, build);
  }
}

std::string cmGlobalFastbuildGenerator::CMakeCmd() const
{
  const auto& lgen = this->LocalGenerators.at(0);
  return lgen->ConvertToOutputFormat(cmSystemTools::GetCMakeCommand(),
                                     cmOutputConverter::SHELL);
}

std::string cmGlobalFastbuildGenerator::FastbuildCmd() const
{
  const auto& lgen = this->LocalGenerators[0];
  if (lgen != nullptr) {
    return lgen->ConvertToOutputFormat(this->FastbuildCommand,
                                       cmOutputConverter::SHELL);
  }
  return "fastbuild";
}

bool cmGlobalFastbuildGenerator::SupportsConsolePool() const
{
  return this->FastbuildSupportsConsolePool;
}

bool cmGlobalFastbuildGenerator::SupportsImplicitOuts() const
{
  return this->FastbuildSupportsImplicitOuts;
}

bool cmGlobalFastbuildGenerator::SupportsManifestRestat() const
{
  return this->FastbuildSupportsManifestRestat;
}

bool cmGlobalFastbuildGenerator::SupportsMultilineDepfile() const
{
  return this->FastbuildSupportsMultilineDepfile;
}

bool cmGlobalFastbuildGenerator::WriteTargetCleanAdditional(std::ostream& os)
{
  const auto& lgr = this->LocalGenerators.at(0);
  std::string cleanScriptRel = "CMakeFiles/clean_additional.cmake";
  std::string cleanScriptAbs =
    cmStrCat(lgr->GetBinaryDirectory(), '/', cleanScriptRel);
  std::vector<std::string> configs =
    this->Makefiles[0]->GetGeneratorConfigs(cmMakefile::IncludeEmptyConfig);

  // Check if there are additional files to clean
  bool empty = true;
  for (auto const& config : configs) {
    auto const it = this->Configs.find(config);
    if (it != this->Configs.end() &&
        !it->second.AdditionalCleanFiles.empty()) {
      empty = false;
      break;
    }
  }
  if (empty) {
    // Remove cmake clean script file if it exists
    cmSystemTools::RemoveFile(cleanScriptAbs);
    return false;
  }

  // Write cmake clean script file
  {
    cmGeneratedFileStream fout(cleanScriptAbs);
    if (!fout) {
      return false;
    }
    fout <<  "NINJA Additional clean files\ncmake_minimum_required(VERSION 3.16)\n";
    for (auto const& config : configs) {
      auto const it = this->Configs.find(config);
      if (it != this->Configs.end() &&
          !it->second.AdditionalCleanFiles.empty()) {
        fout << "\nif(\"${CONFIG}\" STREQUAL \"\" OR \"${CONFIG}\" STREQUAL \""
             << config << "\")\n";
        fout << "  file(REMOVE_RECURSE\n";
        for (std::string const& acf : it->second.AdditionalCleanFiles) {
          fout << "  "
               << cmOutputConverter::EscapeForCMake(
                    this->ConvertToFastbuildPath(acf))
               << '\n';
        }
        fout << "  )\n";
        fout << "endif()\n";
      }
    }
  }
  // Register clean script file
  lgr->GetMakefile()->AddCMakeOutputFile(cleanScriptAbs);

  // Write rule
  {
    cmFastbuildRule rule("CLEAN_ADDITIONAL");
    rule.Command = cmStrCat(
      this->CMakeCmd(), " -DCONFIG=$CONFIG -P ",
      lgr->ConvertToOutputFormat(this->FastbuildOutputPath(cleanScriptRel),
                                 cmOutputConverter::SHELL));
    rule.Description = "Cleaning additional files...";
    rule.Comment = "Rule for cleaning additional files.";
    WriteRule(*this->RulesFileStream, rule);
  }

  // Write build
  {
    cmFastbuildBuild build("CLEAN_ADDITIONAL");
    build.Comment = "Clean additional files.";
    build.Outputs.emplace_back();
    for (auto const& config : configs) {
      build.Outputs.front() = this->BuildAlias(
        this->FastbuildOutputPath(this->GetAdditionalCleanTargetName()), config);
      build.Variables["CONFIG"] = config;
      this->WriteBuild(os, build);
    }
    if (this->IsMultiConfig()) {
      build.Outputs.front() =
        this->FastbuildOutputPath(this->GetAdditionalCleanTargetName());
      build.Variables["CONFIG"] = "";
      this->WriteBuild(os, build);
    }
  }
  // Return success
  return true;
}

void cmGlobalFastbuildGenerator::WriteTargetClean(std::ostream& os)
{
  // -- Additional clean target
  bool additionalFiles = this->WriteTargetCleanAdditional(os);

  // -- Default clean target
  // Write rule
  {
    cmFastbuildRule rule("CLEAN");
    rule.Command = cmStrCat(this->FastbuildCmd(), " $FILE_ARG -t clean $TARGETS");
    rule.Description = "Cleaning all built files...";
    rule.Comment = "Rule for cleaning all built files.";
    WriteRule(*this->RulesFileStream, rule);
  }

  auto const configs = this->Makefiles.front()->GetGeneratorConfigs(
    cmMakefile::IncludeEmptyConfig);

  // Write build
  {
    cmFastbuildBuild build("CLEAN");
    build.Comment = "Clean all the built files.";
    build.Outputs.emplace_back();

    for (auto const& config : configs) {
      build.Outputs.front() = this->BuildAlias(
        this->FastbuildOutputPath(this->GetCleanTargetName()), config);
      if (this->IsMultiConfig()) {
        build.Variables["TARGETS"] =
          cmStrCat(this->BuildAlias(GetByproductsForCleanTargetName(), config),
                   " ", GetByproductsForCleanTargetName());
      }
      build.ExplicitDeps.clear();
      if (additionalFiles) {
        build.ExplicitDeps.push_back(this->BuildAlias(
          this->FastbuildOutputPath(this->GetAdditionalCleanTargetName()),
          config));
      }
      for (auto const& fileConfig : configs) {
        if (fileConfig != config && !this->EnableCrossConfigBuild()) {
          continue;
        }
        if (this->IsMultiConfig()) {
          build.Variables["FILE_ARG"] = cmStrCat(
            /*"-f "*/ "-config",
            cmGlobalFastbuildMultiGenerator::GetFastbuildImplFilename(fileConfig));
        }
        this->WriteBuild(*this->GetImplFileStream(fileConfig), build);
      }
    }

    if (this->EnableCrossConfigBuild()) {
      build.Outputs.front() = this->BuildAlias(
        this->FastbuildOutputPath(this->GetCleanTargetName()), "all");
      build.ExplicitDeps.clear();

      if (additionalFiles) {
        for (auto const& config : this->CrossConfigs) {
          build.ExplicitDeps.push_back(this->BuildAlias(
            this->FastbuildOutputPath(this->GetAdditionalCleanTargetName()),
            config));
        }
      }

      std::vector<std::string> byproducts;
      for (auto const& config : this->CrossConfigs) {
        byproducts.push_back(
          this->BuildAlias(GetByproductsForCleanTargetName(), config));
      }
      byproducts.emplace_back(GetByproductsForCleanTargetName());
      build.Variables["TARGETS"] = cmJoin(byproducts, " ");

      for (auto const& fileConfig : configs) {
        build.Variables["FILE_ARG"] = cmStrCat(
          /*"-f "*/ "-config",
          cmGlobalFastbuildMultiGenerator::GetFastbuildImplFilename(fileConfig));
        this->WriteBuild(*this->GetImplFileStream(fileConfig), build);
      }
    }
  }

  if (this->IsMultiConfig()) {
    cmFastbuildBuild build("phony");
    build.Outputs.emplace_back(
      this->FastbuildOutputPath(this->GetCleanTargetName()));
    build.ExplicitDeps.emplace_back();

    for (auto const& config : configs) {
      build.ExplicitDeps.front() = this->BuildAlias(
        this->FastbuildOutputPath(this->GetCleanTargetName()), config);
      this->WriteBuild(*this->GetConfigFileStream(config), build);
    }

    if (!this->DefaultConfigs.empty()) {
      build.ExplicitDeps.clear();
      for (auto const& config : this->DefaultConfigs) {
        build.ExplicitDeps.push_back(this->BuildAlias(
          this->FastbuildOutputPath(this->GetCleanTargetName()), config));
      }
      this->WriteBuild(*this->GetDefaultFileStream(), build);
    }
  }

  // Write byproducts
  if (this->IsMultiConfig()) {
    cmFastbuildBuild build("phony");
    build.Comment = "Clean byproducts.";
    build.Outputs.emplace_back(
      this->ConvertToFastbuildPath(GetByproductsForCleanTargetName()));
    build.ExplicitDeps = this->ByproductsForCleanTarget;
    this->WriteBuild(os, build);

    for (auto const& config : configs) {
      build.Outputs.front() = this->BuildAlias(
        this->ConvertToFastbuildPath(GetByproductsForCleanTargetName()), config);
      build.ExplicitDeps = this->Configs[config].ByproductsForCleanTarget;
      this->WriteBuild(os, build);
    }
  }
}

void cmGlobalFastbuildGenerator::WriteTargetHelp(std::ostream& os)
{
  {
    cmFastbuildRule rule("HELP");
    rule.Command = cmStrCat(this->FastbuildCmd(), " -t targets");
    rule.Description = "// NINJA All primary targets available:";
    rule.Comment = "// NINJA Rule for printing all primary targets available.";
    WriteRule(*this->RulesFileStream, rule);
  }
  {
    cmFastbuildBuild build("HELP");
    build.Comment = "Print all primary targets available.";
    build.Outputs.push_back(this->FastbuildOutputPath("help"));
    this->WriteBuild(os, build);
  }
}

void cmGlobalFastbuildGenerator::InitOutputPathPrefix()
{
  this->OutputPathPrefix =
    this->LocalGenerators[0]->GetMakefile()->GetSafeDefinition(
      "CMAKE_FASTBUILD_OUTPUT_PATH_PREFIX");
  EnsureTrailingSlash(this->OutputPathPrefix);
}

std::string cmGlobalFastbuildGenerator::FastbuildOutputPath(
  std::string const& path) const
{
  if (!this->HasOutputPathPrefix() || cmSystemTools::FileIsFullPath(path)) {
    return path;
  }
  return cmStrCat(this->OutputPathPrefix, path);
}

void cmGlobalFastbuildGenerator::StripFastbuildOutputPathPrefixAsSuffix(
  std::string& path)
{
  if (path.empty()) {
    return;
  }
  EnsureTrailingSlash(path);
  cmStripSuffixIfExists(path, this->OutputPathPrefix);
}

#if !defined(CMAKE_BOOTSTRAP)

/*

We use the following approach to support Fortran.  Each target already
has a <target>.dir/ directory used to hold intermediate files for CMake.
For each target, a FortranDependInfo.json file is generated by CMake with
information about include directories, module directories, and the locations
the per-target directories for target dependencies.

Compilation of source files within a target is split into the following steps:

1. Preprocess all sources, scan preprocessed output for module dependencies.
   This step is done with independent build statements for each source,
   and can therefore be done in parallel.

    rule Fortran_PREPROCESS
      depfile = $DEP_FILE
      command = gfortran -cpp $DEFINES $INCLUDES $FLAGS -E $in -o $out &&
                cmake -E cmake_ninja_depends \
                  --tdi=FortranDependInfo.json --pp=$out --dep=$DEP_FILE \
                  --obj=$OBJ_FILE --ddi=$DYNDEP_INTERMEDIATE_FILE \
                  --lang=Fortran

    build src.f90-pp.f90 | src.f90.o.ddi: Fortran_PREPROCESS src.f90
      OBJ_FILE = src.f90.o
      DEP_FILE = src.f90.o.d
      DYNDEP_INTERMEDIATE_FILE = src.f90.o.ddi

   The ``cmake -E cmake_ninja_depends`` tool reads the preprocessed output
   and generates the ninja depfile for preprocessor dependencies.  It also
   generates a "ddi" file (in a format private to CMake) that lists the
   object file that compilation will produce along with the module names
   it provides and/or requires.  The "ddi" file is an implicit output
   because it should not appear in "$out" but is generated by the rule.

2. Consolidate the per-source module dependencies saved in the "ddi"
   files from all sources to produce a ninja "dyndep" file, ``Fortran.dd``.

    rule Fortran_DYNDEP
      command = cmake -E cmake_ninja_dyndep \
                  --tdi=FortranDependInfo.json --lang=Fortran --dd=$out $in

    build Fortran.dd: Fortran_DYNDEP src1.f90.o.ddi src2.f90.o.ddi

   The ``cmake -E cmake_ninja_dyndep`` tool reads the "ddi" files from all
   sources in the target and the ``FortranModules.json`` files from targets
   on which the target depends.  It computes dependency edges on compilations
   that require modules to those that provide the modules.  This information
   is placed in the ``Fortran.dd`` file for ninja to load later.  It also
   writes the expected location of modules provided by this target into
   ``FortranModules.json`` for use by dependent targets.

3. Compile all sources after loading dynamically discovered dependencies
   of the compilation build statements from their ``dyndep`` bindings.

    rule Fortran_COMPILE
      command = gfortran $INCLUDES $FLAGS -c $in -o $out

    build src1.f90.o: Fortran_COMPILE src1.f90-pp.f90 || Fortran.dd
      dyndep = Fortran.dd

   The "dyndep" binding tells ninja to load dynamically discovered
   dependency information from ``Fortran.dd``.  This adds information
   such as:

    build src1.f90.o | mod1.mod: dyndep
      restat = 1

   This tells ninja that ``mod1.mod`` is an implicit output of compiling
   the object file ``src1.f90.o``.  The ``restat`` binding tells it that
   the timestamp of the output may not always change.  Additionally:

    build src2.f90.o: dyndep | mod1.mod

   This tells ninja that ``mod1.mod`` is a dependency of compiling the
   object file ``src2.f90.o``.  This ensures that ``src1.f90.o`` and
   ``mod1.mod`` will always be up to date before ``src2.f90.o`` is built
   (because the latter consumes the module).
*/

static std::unique_ptr<cmSourceInfo> cmcmd_cmake_fastbuild_depends_fortran(
  std::string const& arg_tdi, std::string const& arg_pp);

int cmcmd_cmake_fastbuild_depends(std::vector<std::string>::const_iterator argBeg,
                              std::vector<std::string>::const_iterator argEnd)
{
  std::string arg_tdi;
  std::string arg_src;
  std::string arg_pp;
  std::string arg_dep;
  std::string arg_obj;
  std::string arg_ddi;
  std::string arg_lang;
  for (std::string const& arg : cmMakeRange(argBeg, argEnd)) {
    if (cmHasLiteralPrefix(arg, "--tdi=")) {
      arg_tdi = arg.substr(6);
    } else if (cmHasLiteralPrefix(arg, "--src=")) {
      arg_src = arg.substr(6);
    } else if (cmHasLiteralPrefix(arg, "--pp=")) {
      arg_pp = arg.substr(5);
    } else if (cmHasLiteralPrefix(arg, "--dep=")) {
      arg_dep = arg.substr(6);
    } else if (cmHasLiteralPrefix(arg, "--obj=")) {
      arg_obj = arg.substr(6);
    } else if (cmHasLiteralPrefix(arg, "--ddi=")) {
      arg_ddi = arg.substr(6);
    } else if (cmHasLiteralPrefix(arg, "--lang=")) {
      arg_lang = arg.substr(7);
    } else {
      cmSystemTools::Error(
        cmStrCat("-E cmake_fastbuild_depends unknown argument: ", arg));
      return 1;
    }
  }
  if (arg_tdi.empty()) {
    cmSystemTools::Error("-E cmake_fastbuild_depends requires value for --tdi=");
    return 1;
  }
  if (arg_pp.empty()) {
    cmSystemTools::Error("-E cmake_fastbuild_depends requires value for --pp=");
    return 1;
  }
  if (arg_dep.empty()) {
    cmSystemTools::Error("-E cmake_fastbuild_depends requires value for --dep=");
    return 1;
  }
  if (arg_obj.empty()) {
    cmSystemTools::Error("-E cmake_fastbuild_depends requires value for --obj=");
    return 1;
  }
  if (arg_ddi.empty()) {
    cmSystemTools::Error("-E cmake_fastbuild_depends requires value for --ddi=");
    return 1;
  }
  if (arg_lang.empty()) {
    cmSystemTools::Error("-E cmake_fastbuild_depends requires value for --lang=");
    return 1;
  }
  if (arg_src.empty()) {
    arg_src = cmStrCat("<", arg_obj, " input file>");
  }

  std::unique_ptr<cmSourceInfo> info;
  if (arg_lang == "Fortran") {
    info = cmcmd_cmake_fastbuild_depends_fortran(arg_tdi, arg_pp);
  } else {
    cmSystemTools::Error(
      cmStrCat("-E cmake_fastbuild_depends does not understand the ", arg_lang,
               " language"));
    return 1;
  }

  if (!info) {
    // The error message is already expected to have been output.
    return 1;
  }

  info->PrimaryOutput = arg_obj;

  {
    cmGeneratedFileStream depfile(arg_dep);
    depfile << cmSystemTools::ConvertToUnixOutputPath(arg_pp) << ":";
    for (std::string const& include : info->Includes) {
      depfile << " \\\n " << cmSystemTools::ConvertToUnixOutputPath(include);
    }
    depfile << "\n";
  }

  if (!cmScanDepFormat_P1689_Write(arg_ddi, arg_src, *info)) {
    cmSystemTools::Error(
      cmStrCat("-E cmake_fastbuild_depends failed to write ", arg_ddi));
    return 1;
  }
  return 0;
}

std::unique_ptr<cmSourceInfo> cmcmd_cmake_fastbuild_depends_fortran(
  std::string const& arg_tdi, std::string const& arg_pp)
{
  cmFortranCompiler fc;
  std::vector<std::string> includes;
  {
    Json::Value tdio;
    Json::Value const& tdi = tdio;
    {
      cmsys::ifstream tdif(arg_tdi.c_str(), std::ios::in | std::ios::binary);
      Json::Reader reader;
      if (!reader.parse(tdif, tdio, false)) {
        cmSystemTools::Error(
          cmStrCat("-E cmake_fastbuild_depends failed to parse ", arg_tdi,
                   reader.getFormattedErrorMessages()));
        return nullptr;
      }
    }

    Json::Value const& tdi_include_dirs = tdi["include-dirs"];
    if (tdi_include_dirs.isArray()) {
      for (auto const& tdi_include_dir : tdi_include_dirs) {
        includes.push_back(tdi_include_dir.asString());
      }
    }

    Json::Value const& tdi_compiler_id = tdi["compiler-id"];
    fc.Id = tdi_compiler_id.asString();

    Json::Value const& tdi_submodule_sep = tdi["submodule-sep"];
    fc.SModSep = tdi_submodule_sep.asString();

    Json::Value const& tdi_submodule_ext = tdi["submodule-ext"];
    fc.SModExt = tdi_submodule_ext.asString();
  }

  cmFortranSourceInfo finfo;
  std::set<std::string> defines;
  cmFortranParser parser(fc, includes, defines, finfo);
  if (!cmFortranParser_FilePush(&parser, arg_pp.c_str())) {
    cmSystemTools::Error(
      cmStrCat("-E cmake_fastbuild_depends failed to open ", arg_pp));
    return nullptr;
  }
  if (cmFortran_yyparse(parser.Scanner) != 0) {
    // Failed to parse the file.
    return nullptr;
  }

  auto info = cm::make_unique<cmSourceInfo>();
  for (std::string const& provide : finfo.Provides) {
    cmSourceReqInfo src_info;
    src_info.LogicalName = provide;
    src_info.CompiledModulePath = provide;
    info->Provides.emplace_back(src_info);
  }
  for (std::string const& require : finfo.Requires) {
    // Require modules not provided in the same source.
    if (finfo.Provides.count(require)) {
      continue;
    }
    cmSourceReqInfo src_info;
    src_info.LogicalName = require;
    src_info.CompiledModulePath = require;
    info->Requires.emplace_back(src_info);
  }
  for (std::string const& include : finfo.Includes) {
    info->Includes.push_back(include);
  }
  return info;
}

bool cmGlobalFastbuildGenerator::WriteDyndepFile(
  std::string const& dir_top_src, std::string const& dir_top_bld,
  std::string const& dir_cur_src, std::string const& dir_cur_bld,
  std::string const& arg_dd, std::vector<std::string> const& arg_ddis,
  std::string const& module_dir,
  std::vector<std::string> const& linked_target_dirs,
  std::string const& arg_lang, std::string const& arg_modmapfmt)
{
  // Setup path conversions.
  {
    cmStateSnapshot snapshot = this->GetCMakeInstance()->GetCurrentSnapshot();
    snapshot.GetDirectory().SetCurrentSource(dir_cur_src);
    snapshot.GetDirectory().SetCurrentBinary(dir_cur_bld);
    snapshot.GetDirectory().SetRelativePathTopSource(dir_top_src.c_str());
    snapshot.GetDirectory().SetRelativePathTopBinary(dir_top_bld.c_str());
    auto mfd = cm::make_unique<cmMakefile>(this, snapshot);
    auto lgd = this->CreateLocalGenerator(mfd.get());
    this->Makefiles.push_back(std::move(mfd));
    this->LocalGenerators.push_back(std::move(lgd));
  }

  std::vector<cmSourceInfo> objects;
  for (std::string const& arg_ddi : arg_ddis) {
    cmSourceInfo info;
    if (!cmScanDepFormat_P1689_Parse(arg_ddi, &info)) {
      cmSystemTools::Error(
        cmStrCat("-E cmake_fastbuild_dyndep failed to parse ddi file ", arg_ddi));
      return false;
    }
    objects.push_back(std::move(info));
  }

  // Map from module name to module file path, if known.
  std::map<std::string, std::string> mod_files;

  // Populate the module map with those provided by linked targets first.
  for (std::string const& linked_target_dir : linked_target_dirs) {
    std::string const ltmn =
      cmStrCat(linked_target_dir, "/", arg_lang, "Modules.json");
    Json::Value ltm;
    cmsys::ifstream ltmf(ltmn.c_str(), std::ios::in | std::ios::binary);
    Json::Reader reader;
    if (ltmf && !reader.parse(ltmf, ltm, false)) {
      cmSystemTools::Error(cmStrCat("-E cmake_fastbuild_dyndep failed to parse ",
                                    linked_target_dir,
                                    reader.getFormattedErrorMessages()));
      return false;
    }
    if (ltm.isObject()) {
      for (Json::Value::iterator i = ltm.begin(); i != ltm.end(); ++i) {
        mod_files[i.key().asString()] = i->asString();
      }
    }
  }

  // Extend the module map with those provided by this target.
  // We do this after loading the modules provided by linked targets
  // in case we have one of the same name that must be preferred.
  Json::Value tm = Json::objectValue;
  for (cmSourceInfo const& object : objects) {
    for (auto const& p : object.Provides) {
      std::string const mod = cmStrCat(
        module_dir, cmSystemTools::GetFilenameName(p.CompiledModulePath));
      mod_files[p.LogicalName] = mod;
      tm[p.LogicalName] = mod;
    }
  }

  cmGeneratedFileStream ddf(arg_dd);
  ddf << "fastbuild_dyndep_version = 1.0\n";

  {
    cmFastbuildBuild build("dyndep");
    build.Outputs.emplace_back("");
    for (cmSourceInfo const& object : objects) {
      build.Outputs[0] = this->ConvertToFastbuildPath(object.PrimaryOutput);
      build.ImplicitOuts.clear();
      for (auto const& p : object.Provides) {
        build.ImplicitOuts.push_back(
          this->ConvertToFastbuildPath(mod_files[p.LogicalName]));
      }
      build.ImplicitDeps.clear();
      for (auto const& r : object.Requires) {
        auto mit = mod_files.find(r.LogicalName);
        if (mit != mod_files.end()) {
          build.ImplicitDeps.push_back(this->ConvertToFastbuildPath(mit->second));
        }
      }
      build.Variables.clear();
      if (!object.Provides.empty()) {
        build.Variables.emplace("restat", "1");
      }

      if (arg_modmapfmt.empty()) {
        // nothing to do.
      } else {
        std::stringstream mm;
        if (arg_modmapfmt == "gcc") {
          // Documented in GCC's documentation. The format is a series of lines
          // with a module name and the associated filename separated by
          // spaces. The first line may use `$root` as the module name to
          // specify a "repository root". That is used to anchor any relative
          // paths present in the file (CMake should never generate any).

          // Write the root directory to use for module paths.
          mm << "$root .\n";

          for (auto const& l : object.Provides) {
            auto m = mod_files.find(l.LogicalName);
            if (m != mod_files.end()) {
              mm << l.LogicalName << " " << this->ConvertToFastbuildPath(m->second)
                 << "\n";
            }
          }
          for (auto const& r : object.Requires) {
            auto m = mod_files.find(r.LogicalName);
            if (m != mod_files.end()) {
              mm << r.LogicalName << " " << this->ConvertToFastbuildPath(m->second)
                 << "\n";
            }
          }
        } else {
          cmSystemTools::Error(
            cmStrCat("-E cmake_fastbuild_dyndep does not understand the ",
                     arg_modmapfmt, " module map format"));
          return false;
        }

        // XXX(modmap): If changing this path construction, change
        // `cmNinjaTargetGenerator::WriteObjectBuildStatements` to generate the
        // corresponding file path.
        cmGeneratedFileStream mmf(cmStrCat(object.PrimaryOutput, ".modmap"));
        mmf << mm.str();
      }

      this->WriteBuild(ddf, build);
    }
  }

  // Store the map of modules provided by this target in a file for
  // use by dependents that reference this target in linked-target-dirs.
  std::string const target_mods_file = cmStrCat(
    cmSystemTools::GetFilenamePath(arg_dd), '/', arg_lang, "Modules.json");
  cmGeneratedFileStream tmf(target_mods_file);
  tmf << tm;

  return true;
}

int cmcmd_cmake_fastbuild_dyndep(std::vector<std::string>::const_iterator argBeg,
                             std::vector<std::string>::const_iterator argEnd)
{
  std::vector<std::string> arg_full =
    cmSystemTools::HandleResponseFile(argBeg, argEnd);

  std::string arg_dd;
  std::string arg_lang;
  std::string arg_tdi;
  std::string arg_modmapfmt;
  std::vector<std::string> arg_ddis;
  for (std::string const& arg : arg_full) {
    if (cmHasLiteralPrefix(arg, "--tdi=")) {
      arg_tdi = arg.substr(6);
    } else if (cmHasLiteralPrefix(arg, "--lang=")) {
      arg_lang = arg.substr(7);
    } else if (cmHasLiteralPrefix(arg, "--dd=")) {
      arg_dd = arg.substr(5);
    } else if (cmHasLiteralPrefix(arg, "--modmapfmt=")) {
      arg_modmapfmt = arg.substr(12);
    } else if (!cmHasLiteralPrefix(arg, "--") &&
               cmHasLiteralSuffix(arg, ".ddi")) {
      arg_ddis.push_back(arg);
    } else {
      cmSystemTools::Error(
        cmStrCat("-E cmake_fastbuild_dyndep unknown argument: ", arg));
      return 1;
    }
  }
  if (arg_tdi.empty()) {
    cmSystemTools::Error("-E cmake_fastbuild_dyndep requires value for --tdi=");
    return 1;
  }
  if (arg_lang.empty()) {
    cmSystemTools::Error("-E cmake_fastbuild_dyndep requires value for --lang=");
    return 1;
  }
  if (arg_dd.empty()) {
    cmSystemTools::Error("-E cmake_fastbuild_dyndep requires value for --dd=");
    return 1;
  }

  Json::Value tdio;
  Json::Value const& tdi = tdio;
  {
    cmsys::ifstream tdif(arg_tdi.c_str(), std::ios::in | std::ios::binary);
    Json::Reader reader;
    if (!reader.parse(tdif, tdio, false)) {
      cmSystemTools::Error(cmStrCat("-E cmake_fastbuild_dyndep failed to parse ",
                                    arg_tdi,
                                    reader.getFormattedErrorMessages()));
      return 1;
    }
  }

  std::string const dir_cur_bld = tdi["dir-cur-bld"].asString();
  std::string const dir_cur_src = tdi["dir-cur-src"].asString();
  std::string const dir_top_bld = tdi["dir-top-bld"].asString();
  std::string const dir_top_src = tdi["dir-top-src"].asString();
  std::string module_dir = tdi["module-dir"].asString();
  if (!module_dir.empty() && !cmHasLiteralSuffix(module_dir, "/")) {
    module_dir += '/';
  }
  std::vector<std::string> linked_target_dirs;
  Json::Value const& tdi_linked_target_dirs = tdi["linked-target-dirs"];
  if (tdi_linked_target_dirs.isArray()) {
    for (auto const& tdi_linked_target_dir : tdi_linked_target_dirs) {
      linked_target_dirs.push_back(tdi_linked_target_dir.asString());
    }
  }

  cmake cm(cmake::RoleInternal, cmState::Unknown);
  cm.SetHomeDirectory(dir_top_src);
  cm.SetHomeOutputDirectory(dir_top_bld);
  auto ggd = cm.CreateGlobalGenerator("Fastbuild");
  if (!ggd ||
      !cm::static_reference_cast<cmGlobalFastbuildGenerator>(ggd).WriteDyndepFile(
        dir_top_src, dir_top_bld, dir_cur_src, dir_cur_bld, arg_dd, arg_ddis,
        module_dir, linked_target_dirs, arg_lang, arg_modmapfmt)) {
    return 1;
  }
  return 0;
}

#endif

bool cmGlobalFastbuildGenerator::EnableCrossConfigBuild() const
{
  return !this->CrossConfigs.empty();
}

void cmGlobalFastbuildGenerator::AppendDirectoryForConfig(
  const std::string& prefix, const std::string& config,
  const std::string& suffix, std::string& dir)
{
  if (!config.empty() && this->IsMultiConfig()) {
    dir += cmStrCat(prefix, config, suffix);
  }
}

std::set<std::string> cmGlobalFastbuildGenerator::GetCrossConfigs(
  const std::string& fileConfig) const
{
  auto result = this->CrossConfigs;
  result.insert(fileConfig);
  return result;
}

bool cmGlobalFastbuildGenerator::IsSingleConfigUtility(
  cmGeneratorTarget const* target) const
{
  return target->GetType() == cmStateEnums::UTILITY &&
    !this->PerConfigUtilityTargets.count(target->GetName());
}

const char* cmGlobalFastbuildMultiGenerator::FASTBUILD_COMMON_FILE =
  "CMakeFiles/common.bff";
const char* cmGlobalFastbuildMultiGenerator::FASTBUILD_COMMON_FILE_NAME = "common.bff";
const char* cmGlobalFastbuildMultiGenerator::FASTBUILD_FILE_EXTENSION = ".bff";

cmGlobalFastbuildMultiGenerator::cmGlobalFastbuildMultiGenerator(cmake* cm)
  : cmGlobalFastbuildGenerator(cm)
{
  cm->GetState()->SetIsGeneratorMultiConfig(true);
  cm->GetState()->SetFastbuildMulti(true);
}

void cmGlobalFastbuildMultiGenerator::GetDocumentation(cmDocumentationEntry& entry)
{
  entry.Name = cmGlobalFastbuildMultiGenerator::GetActualName();
  entry.Brief = "Generates fbuild-<Config>.bff files.";
}

std::string cmGlobalFastbuildMultiGenerator::ExpandCFGIntDir(
  const std::string& str, const std::string& config) const
{
  std::string result = str;
  cmSystemTools::ReplaceString(result, this->GetCMakeCFGIntDir(), config);
  return result;
}

bool cmGlobalFastbuildMultiGenerator::OpenBuildFileStreams()
{
  if (!this->OpenFileStream(this->CommonFileStream,
                            cmGlobalFastbuildMultiGenerator::FASTBUILD_COMMON_FILE)) {
    return false;
  }

  if (!this->OpenFileStream(this->DefaultFileStream, FASTBUILD_BUILD_FILE)) {
    return false;
  }
  *this->DefaultFileStream << "// Build using rules for '"
                           << this->DefaultFileConfig << "'.\n\n"
                           << "#include "
                           << "\""
                           << GetFastbuildImplFilename(this->DefaultFileConfig)
                           << "\""
                           << "\n\n";

  // Write a comment about this file.
  *this->CommonFileStream
    << "// NINJA This file contains build statements common to all "
       "configurations.\n\n";

  auto const& configs =
    this->Makefiles[0]->GetGeneratorConfigs(cmMakefile::IncludeEmptyConfig);
  return std::all_of(
    configs.begin(), configs.end(), [this](std::string const& config) -> bool {
      // Open impl file.
      if (!this->OpenFileStream(this->ImplFileStreams[config],
                                GetFastbuildImplFilename(config))) {
        return false;
      }

      // Write a comment about this file.
      *this->ImplFileStreams[config]
        << "// NINJA This file contains build statements specific to the \"" << config
        << "\"\n// configuration.\n\n";

      // Open config file.
      if (!this->OpenFileStream(this->ConfigFileStreams[config],
                                GetFastbuildConfigFilename(config))) {
        return false;
      }

      // Write a comment about this file.
      *this->ConfigFileStreams[config]
        << "// This file contains aliases specific to the \"" << config
        << "\"\n// configuration.\n\n"
        << "#include "
        << "\"" << GetFastbuildImplFilename(config)
        << "\""
        << "\n\n";

      return true;
    });
}

void cmGlobalFastbuildMultiGenerator::CloseBuildFileStreams()
{
  if (this->CommonFileStream) {
    this->CommonFileStream.reset();
  } else {
    cmSystemTools::Error("Common file stream was not open.");
  }

  if (this->DefaultFileStream) {
    this->DefaultFileStream.reset();
  } // No error if it wasn't open

  for (auto const& config : this->Makefiles[0]->GetGeneratorConfigs(
         cmMakefile::IncludeEmptyConfig)) {
    if (this->ImplFileStreams[config]) {
      this->ImplFileStreams[config].reset();
    } else {
      cmSystemTools::Error(
        cmStrCat("Impl file stream for \"", config, "\" was not open."));
    }
    if (this->ConfigFileStreams[config]) {
      this->ConfigFileStreams[config].reset();
    } else {
      cmSystemTools::Error(
        cmStrCat("Config file stream for \"", config, "\" was not open."));
    }
  }
}

void cmGlobalFastbuildMultiGenerator::AppendFastbuildFileArgument(
  GeneratedMakeCommand& command, const std::string& config) const
{
  if (!config.empty()) {
    //command.Add("-f");
    command.Add("-config");
    command.Add(GetFastbuildConfigFilename(config));
  }
}

std::string cmGlobalFastbuildMultiGenerator::GetFastbuildImplFilename(
  const std::string& config)
{
  return cmStrCat("CMakeFiles/impl-", config,
                  cmGlobalFastbuildMultiGenerator::FASTBUILD_FILE_EXTENSION);
}

std::string cmGlobalFastbuildMultiGenerator::GetFastbuildConfigFilename(
  const std::string& config)
{
  return cmStrCat("fbuild-", config,
                  cmGlobalFastbuildMultiGenerator::FASTBUILD_FILE_EXTENSION);
}

void cmGlobalFastbuildMultiGenerator::AddRebuildManifestOutputs(
  cmFastbuildDeps& outputs) const
{
  for (auto const& config : this->Makefiles.front()->GetGeneratorConfigs(
         cmMakefile::IncludeEmptyConfig)) {
    outputs.push_back(this->FastbuildOutputPath(GetFastbuildImplFilename(config)));
    outputs.push_back(this->FastbuildOutputPath(GetFastbuildConfigFilename(config)));
  }
  if (!this->DefaultFileConfig.empty()) {
    outputs.push_back(this->FastbuildOutputPath(FASTBUILD_BUILD_FILE));
  }
}

void cmGlobalFastbuildMultiGenerator::GetQtAutoGenConfigs(
  std::vector<std::string>& configs) const
{
  auto allConfigs =
    this->Makefiles[0]->GetGeneratorConfigs(cmMakefile::IncludeEmptyConfig);
  configs.insert(configs.end(), cm::cbegin(allConfigs), cm::cend(allConfigs));
}

bool cmGlobalFastbuildMultiGenerator::InspectConfigTypeVariables()
{
  std::vector<std::string> configsVec;
  cmExpandList(
    this->Makefiles.front()->GetSafeDefinition("CMAKE_CONFIGURATION_TYPES"),
    configsVec);
  if (configsVec.empty()) {
    configsVec.emplace_back();
  }
  std::set<std::string> configs(configsVec.cbegin(), configsVec.cend());

  this->DefaultFileConfig =
    this->Makefiles.front()->GetSafeDefinition("CMAKE_DEFAULT_BUILD_TYPE");
  if (this->DefaultFileConfig.empty()) {
    this->DefaultFileConfig = configsVec.front();
  }
  if (!configs.count(this->DefaultFileConfig)) {
    std::ostringstream msg;
    msg << "The configuration specified by "
        << "CMAKE_DEFAULT_BUILD_TYPE (" << this->DefaultFileConfig
        << ") is not present in CMAKE_CONFIGURATION_TYPES";
    this->GetCMakeInstance()->IssueMessage(MessageType::FATAL_ERROR,
                                           msg.str());
    return false;
  }

  std::vector<std::string> crossConfigsVec;
  cmExpandList(
    this->Makefiles.front()->GetSafeDefinition("CMAKE_CROSS_CONFIGS"),
    crossConfigsVec);
  auto crossConfigs = ListSubsetWithAll(configs, configs, crossConfigsVec);
  if (!crossConfigs) {
    std::ostringstream msg;
    msg << "CMAKE_CROSS_CONFIGS is not a subset of "
        << "CMAKE_CONFIGURATION_TYPES";
    this->GetCMakeInstance()->IssueMessage(MessageType::FATAL_ERROR,
                                           msg.str());
    return false;
  }
  this->CrossConfigs = *crossConfigs;

  auto defaultConfigsString =
    this->Makefiles.front()->GetSafeDefinition("CMAKE_DEFAULT_CONFIGS");
  if (defaultConfigsString.empty()) {
    defaultConfigsString = this->DefaultFileConfig;
  }
  if (!defaultConfigsString.empty() &&
      defaultConfigsString != this->DefaultFileConfig &&
      (this->DefaultFileConfig.empty() || this->CrossConfigs.empty())) {
    std::ostringstream msg;
    msg << "CMAKE_DEFAULT_CONFIGS cannot be used without "
        << "CMAKE_DEFAULT_BUILD_TYPE or CMAKE_CROSS_CONFIGS";
    this->GetCMakeInstance()->IssueMessage(MessageType::FATAL_ERROR,
                                           msg.str());
    return false;
  }

  std::vector<std::string> defaultConfigsVec;
  cmExpandList(defaultConfigsString, defaultConfigsVec);
  if (!this->DefaultFileConfig.empty()) {
    auto defaultConfigs =
      ListSubsetWithAll(this->GetCrossConfigs(this->DefaultFileConfig),
                        this->CrossConfigs, defaultConfigsVec);
    if (!defaultConfigs) {
      std::ostringstream msg;
      msg << "CMAKE_DEFAULT_CONFIGS is not a subset of CMAKE_CROSS_CONFIGS";
      this->GetCMakeInstance()->IssueMessage(MessageType::FATAL_ERROR,
                                             msg.str());
      return false;
    }
    this->DefaultConfigs = *defaultConfigs;
  }

  return true;
}

std::string cmGlobalFastbuildMultiGenerator::GetDefaultBuildConfig() const
{
  return "";
}

std::string cmGlobalFastbuildMultiGenerator::OrderDependsTargetForTarget(
  cmGeneratorTarget const* target, const std::string& config) const
{
  return cmStrCat("cmake_object_order_depends_target_", target->GetName(), '_',
                  cmSystemTools::UpperCase(config));
}
