#pragma once

#include "parser.h"
#include "demogobbler/packet_netmessages.h"

void demogobbler_parse_packetentities(parser* thisptr, struct demogobbler_svc_packet_entities* message);
