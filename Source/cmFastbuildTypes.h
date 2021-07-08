/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

enum cmFastbuildTargetDepends
{
  DependOnTargetArtifact,
  DependOnTargetOrdering
};

using cmFastbuildDeps = std::vector<std::string>;
using cmFastbuildOuts = std::set<std::string>;
using cmFastbuildVars = std::map<std::string, std::string>;

class cmFastbuildRule
{
public:
  cmFastbuildRule(std::string name)
    : Name(std::move(name))
  {
  }

  std::string Name;
  std::string Command;
  std::string Description;
  std::string Comment;
  std::string DepFile;
  std::string DepType;
  std::string RspFile;
  std::string RspContent;
  std::string Restat;
  bool Generator = false;
};

class cmFastbuildBuild
{
public:
  cmFastbuildBuild() = default;
  cmFastbuildBuild(std::string rule)
    : Rule(std::move(rule))
  {
  }

  std::string Comment;
  std::string Rule;
  cmFastbuildDeps Outputs;
  cmFastbuildDeps ImplicitOuts;
  cmFastbuildDeps ExplicitDeps;
  cmFastbuildDeps ImplicitDeps;
  cmFastbuildDeps OrderOnlyDeps;
  cmFastbuildVars Variables;
  std::string RspFile;
};
