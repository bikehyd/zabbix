/* 
** ZABBIX
** Copyright (C) 2000-2005 SIA Zabbix
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**/

#if !defined(SYSINFO_SIMPLE_H_INCLUDED)

#include "sysinfo.h"

extern ZBX_METRIC	parameters_simple[];

int	CHECK_SERVICE_PERF(const char *cmd, const char *param, unsigned flags, AGENT_RESULT *result);
int	CHECK_SERVICE(const char *cmd, const char *param, unsigned flags, AGENT_RESULT *result);

#endif /* SYSINFO_SIMPLE_H_INCLUDED */
