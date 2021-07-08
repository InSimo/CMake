/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */

#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <string>

#include "cmLinkLineDeviceComputer.h"

class cmGlobalFastbuildGenerator;
class cmOutputConverter;
class cmStateDirectory;

class cmFastbuildLinkLineDeviceComputer : public cmLinkLineDeviceComputer
{
public:
  cmFastbuildLinkLineDeviceComputer(cmOutputConverter* outputConverter,
                                cmStateDirectory const& stateDir,
                                cmGlobalFastbuildGenerator const* gg);

  cmFastbuildLinkLineDeviceComputer(cmFastbuildLinkLineDeviceComputer const&) = delete;
  cmFastbuildLinkLineDeviceComputer& operator=(
    cmFastbuildLinkLineDeviceComputer const&) = delete;

  std::string ConvertToLinkReference(std::string const& input) const override;

private:
  cmGlobalFastbuildGenerator const* GG;
};
