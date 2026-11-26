#pragma once

class QString;

namespace chatterino {

struct CommandContext;

}  // namespace chatterino

namespace chatterino::commands {

QString openDropsDialog(const CommandContext &ctx);
QString toggleWatchChannel(const CommandContext &ctx);
QString waitForStreamCommand(const CommandContext &ctx);
QString claimChannelPointsBonus(const CommandContext &ctx);

}  // namespace chatterino::commands
