/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering
  Copyright 2010 Maarten Lankhorst

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <errno.h>

#include "dbus-unit.h"
#include "dbus-swap.h"
#include "dbus-execute.h"
#include "dbus-common.h"

#define BUS_SWAP_INTERFACE                                              \
        " <interface name=\"org.freedesktop.systemd1.Swap\">\n"         \
        "  <property name=\"What\" type=\"s\" access=\"read\"/>\n"      \
        "  <property name=\"Priority\" type=\"i\" access=\"read\"/>\n"  \
        "  <property name=\"TimeoutUSec\" type=\"t\" access=\"read\"/>\n" \
        BUS_EXEC_COMMAND_INTERFACE("ExecActivate")                      \
        BUS_EXEC_COMMAND_INTERFACE("ExecDeactivate")                    \
        BUS_EXEC_CONTEXT_INTERFACE                                      \
        "  <property name=\"ControlPID\" type=\"u\" access=\"read\"/>\n" \
        " </interface>\n"

#define INTROSPECTION                                                   \
        DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE                       \
        "<node>\n"                                                      \
        BUS_UNIT_INTERFACE                                              \
        BUS_SWAP_INTERFACE                                              \
        BUS_PROPERTIES_INTERFACE                                        \
        BUS_PEER_INTERFACE                                              \
        BUS_INTROSPECTABLE_INTERFACE                                    \
        "</node>\n"

#define INTERFACES_LIST                              \
        BUS_UNIT_INTERFACES_LIST                     \
        "org.freedesktop.systemd1.Swap\0"

const char bus_swap_interface[] _introspect_("Swap") = BUS_SWAP_INTERFACE;

const char bus_swap_invalidating_properties[] =
        "What\0"
        "Priority\0"
        "ExecActivate\0"
        "ExecDeactivate\0"
        "ControlPID\0";

static int bus_swap_append_priority(DBusMessageIter *i, const char *property, void *data) {
        Swap *s = data;
        dbus_int32_t j;

        assert(i);
        assert(property);
        assert(s);

        if (s->from_proc_swaps)
                j = s->parameters_proc_swaps.priority;
        else if (s->from_fragment)
                j = s->parameters_fragment.priority;
        else if (s->from_etc_fstab)
                j = s->parameters_etc_fstab.priority;
        else
                j = -1;

        if (!dbus_message_iter_append_basic(i, DBUS_TYPE_INT32, &j))
                return -ENOMEM;

        return 0;
}

DBusHandlerResult bus_swap_message_handler(Unit *u, DBusConnection *c, DBusMessage *message) {
        const BusProperty properties[] = {
                BUS_UNIT_PROPERTIES,
                { "org.freedesktop.systemd1.Swap", "What",       bus_property_append_string, "s", u->swap.what          },
                { "org.freedesktop.systemd1.Swap", "Priority",   bus_swap_append_priority,   "i", u                     },
                BUS_EXEC_COMMAND_PROPERTY("org.freedesktop.systemd1.Swap", u->swap.exec_command+SWAP_EXEC_ACTIVATE,   "ExecActivate"),
                BUS_EXEC_COMMAND_PROPERTY("org.freedesktop.systemd1.Swap", u->swap.exec_command+SWAP_EXEC_DEACTIVATE, "ExecDeactivate"),
                BUS_EXEC_CONTEXT_PROPERTIES("org.freedesktop.systemd1.Swap", u->swap.exec_context),
                { "org.freedesktop.systemd1.Swap", "ControlPID", bus_property_append_pid,    "u", &u->swap.control_pid },
                { NULL, NULL, NULL, NULL, NULL }
        };

        return bus_default_message_handler(c, message, INTROSPECTION, INTERFACES_LIST, properties);
}
