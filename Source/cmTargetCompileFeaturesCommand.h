/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#ifndef cmTargetCompileFeaturesCommand_h
#define cmTargetCompileFeaturesCommand_h

#include "cmConfigure.h" // IWYU pragma: keep

#include <string>
#include <vector>

#include <cm/memory>

#include "cmCommand.h"
#include "cmTargetPropCommandBase.h"

class cmExecutionStatus;
class cmTarget;

class cmTargetCompileFeaturesCommand : public cmTargetPropCommandBase
{
  std::unique_ptr<cmCommand> Clone() override
  {
    return cm::make_unique<cmTargetCompileFeaturesCommand>();
  }

  bool InitialPass(std::vector<std::string> const& args,
                   cmExecutionStatus& status) override;

private:
  void HandleMissingTarget(const std::string& name) override;

  bool HandleDirectContent(cmTarget* tgt,
                           const std::vector<std::string>& content,
                           bool prepend, bool system) override;
  std::string Join(const std::vector<std::string>& content) override;
};

#endif
