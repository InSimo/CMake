/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */

#include "cmFastbuildLinkLineDeviceComputer.h"

#include "cmGlobalFastbuildGenerator.h"

cmFastbuildLinkLineDeviceComputer::cmFastbuildLinkLineDeviceComputer(
  cmOutputConverter* outputConverter, cmStateDirectory const& stateDir,
  cmGlobalFastbuildGenerator const* gg)
  : cmLinkLineDeviceComputer(outputConverter, stateDir)
  , GG(gg)
{
}

std::string cmFastbuildLinkLineDeviceComputer::ConvertToLinkReference(
  std::string const& lib) const
{
  return this->GG->ConvertToFastbuildPath(lib);
}
