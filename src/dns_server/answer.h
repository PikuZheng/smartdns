/*************************************************************************
 *
 * Copyright (C) 2018-2025 Ruilin Peng (Nick) <pymumu@gmail.com>.
 *
 * smartdns is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * smartdns is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _DNS_SERVER_ANSWER_
#define _DNS_SERVER_ANSWER_

#include "dns_server.h"

#ifdef __cplusplus
extern "C" {
#endif /*__cplusplus */

int _dns_server_process_answer(struct dns_request *request, const char *domain, struct dns_packet *packet,
							   unsigned int result_flag, int *need_passthrouh);

#ifdef __cplusplus
}
#endif /*__cplusplus */
#endif
