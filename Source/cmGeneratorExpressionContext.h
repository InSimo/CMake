/*============================================================================
  CMake - Cross Platform Makefile Generator
  Copyright 2012 Stephen Kelly <steveire@gmail.com>

  Distributed under the OSI-approved BSD License (the "License");
  see accompanying file Copyright.txt for details.

  This software is distributed WITHOUT ANY WARRANTY; without even the
  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the License for more information.
============================================================================*/
#ifndef cmGeneratorExpressionContext_h
#define cmGeneratorExpressionContext_h

#include <set>
#include <map>
#include <string>

#include "cmListFileCache.h"

class cmTarget;

//----------------------------------------------------------------------------
struct cmGeneratorExpressionContext
{
  cmGeneratorExpressionContext();

  cmListFileBacktrace Backtrace;
  std::set<cmTarget*> DependTargets;
  std::set<cmTarget const*> AllTargets;
  std::set<std::string> SeenTargetProperties;
  std::set<cmTarget const*> SourceSensitiveTargets;
  std::map<cmTarget const*, std::map<std::string, std::string> >
                                                          MaxLanguageStandard;
  cmMakefile *Makefile;
  std::string Config;
  std::string Language;
  cmTarget const* HeadTarget; // The target whose property is being evaluated.
  cmTarget const* CurrentTarget; // The dependent of HeadTarget which appears
                                 // directly or indirectly in the property.
  bool Quiet;
  bool HadError;
  bool HadContextSensitiveCondition;
  bool HadHeadSensitiveCondition;
  bool EvaluateForBuildsystem;
};

#endif
