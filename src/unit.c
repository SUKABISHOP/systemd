/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

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

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/poll.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

#include "set.h"
#include "unit.h"
#include "macro.h"
#include "strv.h"
#include "load-fragment.h"
#include "load-dropin.h"
#include "log.h"
#include "unit-name.h"
#include "specifier.h"
#include "dbus-unit.h"
#include "special.h"
#include "cgroup-util.h"
#include "missing.h"

const UnitVTable * const unit_vtable[_UNIT_TYPE_MAX] = {
        [UNIT_SERVICE] = &service_vtable,
        [UNIT_TIMER] = &timer_vtable,
        [UNIT_SOCKET] = &socket_vtable,
        [UNIT_TARGET] = &target_vtable,
        [UNIT_DEVICE] = &device_vtable,
        [UNIT_MOUNT] = &mount_vtable,
        [UNIT_AUTOMOUNT] = &automount_vtable,
        [UNIT_SNAPSHOT] = &snapshot_vtable,
        [UNIT_SWAP] = &swap_vtable,
        [UNIT_PATH] = &path_vtable
};

Unit *unit_new(Manager *m) {
        Unit *u;

        assert(m);

        if (!(u = new0(Unit, 1)))
                return NULL;

        if (!(u->meta.names = set_new(string_hash_func, string_compare_func))) {
                free(u);
                return NULL;
        }

        u->meta.manager = m;
        u->meta.type = _UNIT_TYPE_INVALID;
        u->meta.deserialized_job = _JOB_TYPE_INVALID;
        u->meta.default_dependencies = true;

        return u;
}

bool unit_has_name(Unit *u, const char *name) {
        assert(u);
        assert(name);

        return !!set_get(u->meta.names, (char*) name);
}

int unit_add_name(Unit *u, const char *text) {
        UnitType t;
        char *s, *i = NULL;
        int r;

        assert(u);
        assert(text);

        if (unit_name_is_template(text)) {
                if (!u->meta.instance)
                        return -EINVAL;

                s = unit_name_replace_instance(text, u->meta.instance);
        } else
                s = strdup(text);

        if (!s)
                return -ENOMEM;

        if (!unit_name_is_valid(s, false)) {
                r = -EINVAL;
                goto fail;
        }

        assert_se((t = unit_name_to_type(s)) >= 0);

        if (u->meta.type != _UNIT_TYPE_INVALID && t != u->meta.type) {
                r = -EINVAL;
                goto fail;
        }

        if ((r = unit_name_to_instance(s, &i)) < 0)
                goto fail;

        if (i && unit_vtable[t]->no_instances) {
                r = -EINVAL;
                goto fail;
        }

        /* Ensure that this unit is either instanced or not instanced,
         * but not both. */
        if (u->meta.type != _UNIT_TYPE_INVALID && !u->meta.instance != !i) {
                r = -EINVAL;
                goto fail;
        }

        if (unit_vtable[t]->no_alias &&
            !set_isempty(u->meta.names) &&
            !set_get(u->meta.names, s)) {
                r = -EEXIST;
                goto fail;
        }

        if (hashmap_size(u->meta.manager->units) >= MANAGER_MAX_NAMES) {
                r = -E2BIG;
                goto fail;
        }

        if ((r = set_put(u->meta.names, s)) < 0) {
                if (r == -EEXIST)
                        r = 0;
                goto fail;
        }

        if ((r = hashmap_put(u->meta.manager->units, s, u)) < 0) {
                set_remove(u->meta.names, s);
                goto fail;
        }

        if (u->meta.type == _UNIT_TYPE_INVALID) {

                u->meta.type = t;
                u->meta.id = s;
                u->meta.instance = i;

                LIST_PREPEND(Meta, units_per_type, u->meta.manager->units_per_type[t], &u->meta);

                if (UNIT_VTABLE(u)->init)
                        UNIT_VTABLE(u)->init(u);
        } else
                free(i);

        unit_add_to_dbus_queue(u);
        return 0;

fail:
        free(s);
        free(i);

        return r;
}

int unit_choose_id(Unit *u, const char *name) {
        char *s, *t = NULL, *i;
        int r;

        assert(u);
        assert(name);

        if (unit_name_is_template(name)) {

                if (!u->meta.instance)
                        return -EINVAL;

                if (!(t = unit_name_replace_instance(name, u->meta.instance)))
                        return -ENOMEM;

                name = t;
        }

        /* Selects one of the names of this unit as the id */
        s = set_get(u->meta.names, (char*) name);
        free(t);

        if (!s)
                return -ENOENT;

        if ((r = unit_name_to_instance(s, &i)) < 0)
                return r;

        u->meta.id = s;

        free(u->meta.instance);
        u->meta.instance = i;

        unit_add_to_dbus_queue(u);

        return 0;
}

int unit_set_description(Unit *u, const char *description) {
        char *s;

        assert(u);

        if (!(s = strdup(description)))
                return -ENOMEM;

        free(u->meta.description);
        u->meta.description = s;

        unit_add_to_dbus_queue(u);
        return 0;
}

bool unit_check_gc(Unit *u) {
        assert(u);

        if (u->meta.load_state == UNIT_STUB)
                return true;

        if (UNIT_VTABLE(u)->no_gc)
                return true;

        if (u->meta.no_gc)
                return true;

        if (u->meta.job)
                return true;

        if (unit_active_state(u) != UNIT_INACTIVE)
                return true;

        if (UNIT_VTABLE(u)->check_gc)
                if (UNIT_VTABLE(u)->check_gc(u))
                        return true;

        return false;
}

void unit_add_to_load_queue(Unit *u) {
        assert(u);
        assert(u->meta.type != _UNIT_TYPE_INVALID);

        if (u->meta.load_state != UNIT_STUB || u->meta.in_load_queue)
                return;

        LIST_PREPEND(Meta, load_queue, u->meta.manager->load_queue, &u->meta);
        u->meta.in_load_queue = true;
}

void unit_add_to_cleanup_queue(Unit *u) {
        assert(u);

        if (u->meta.in_cleanup_queue)
                return;

        LIST_PREPEND(Meta, cleanup_queue, u->meta.manager->cleanup_queue, &u->meta);
        u->meta.in_cleanup_queue = true;
}

void unit_add_to_gc_queue(Unit *u) {
        assert(u);

        if (u->meta.in_gc_queue || u->meta.in_cleanup_queue)
                return;

        if (unit_check_gc(u))
                return;

        LIST_PREPEND(Meta, gc_queue, u->meta.manager->gc_queue, &u->meta);
        u->meta.in_gc_queue = true;

        u->meta.manager->n_in_gc_queue ++;

        if (u->meta.manager->gc_queue_timestamp <= 0)
                u->meta.manager->gc_queue_timestamp = now(CLOCK_MONOTONIC);
}

void unit_add_to_dbus_queue(Unit *u) {
        assert(u);
        assert(u->meta.type != _UNIT_TYPE_INVALID);

        if (u->meta.load_state == UNIT_STUB || u->meta.in_dbus_queue)
                return;

        /* Shortcut things if nobody cares */
        if (!bus_has_subscriber(u->meta.manager)) {
                u->meta.sent_dbus_new_signal = true;
                return;
        }

        LIST_PREPEND(Meta, dbus_queue, u->meta.manager->dbus_unit_queue, &u->meta);
        u->meta.in_dbus_queue = true;
}

static void bidi_set_free(Unit *u, Set *s) {
        Iterator i;
        Unit *other;

        assert(u);

        /* Frees the set and makes sure we are dropped from the
         * inverse pointers */

        SET_FOREACH(other, s, i) {
                UnitDependency d;

                for (d = 0; d < _UNIT_DEPENDENCY_MAX; d++)
                        set_remove(other->meta.dependencies[d], u);

                unit_add_to_gc_queue(other);
        }

        set_free(s);
}

void unit_free(Unit *u) {
        UnitDependency d;
        Iterator i;
        char *t;

        assert(u);

        bus_unit_send_removed_signal(u);

        if (u->meta.load_state != UNIT_STUB)
                if (UNIT_VTABLE(u)->done)
                        UNIT_VTABLE(u)->done(u);

        SET_FOREACH(t, u->meta.names, i)
                hashmap_remove_value(u->meta.manager->units, t, u);

        if (u->meta.job)
                job_free(u->meta.job);

        for (d = 0; d < _UNIT_DEPENDENCY_MAX; d++)
                bidi_set_free(u, u->meta.dependencies[d]);

        if (u->meta.type != _UNIT_TYPE_INVALID)
                LIST_REMOVE(Meta, units_per_type, u->meta.manager->units_per_type[u->meta.type], &u->meta);

        if (u->meta.in_load_queue)
                LIST_REMOVE(Meta, load_queue, u->meta.manager->load_queue, &u->meta);

        if (u->meta.in_dbus_queue)
                LIST_REMOVE(Meta, dbus_queue, u->meta.manager->dbus_unit_queue, &u->meta);

        if (u->meta.in_cleanup_queue)
                LIST_REMOVE(Meta, cleanup_queue, u->meta.manager->cleanup_queue, &u->meta);

        if (u->meta.in_gc_queue) {
                LIST_REMOVE(Meta, gc_queue, u->meta.manager->gc_queue, &u->meta);
                u->meta.manager->n_in_gc_queue--;
        }

        cgroup_bonding_free_list(u->meta.cgroup_bondings, u->meta.manager->n_serializing <= 0);

        free(u->meta.description);
        free(u->meta.fragment_path);

        set_free_free(u->meta.names);

        condition_free_list(u->meta.conditions);

        free(u->meta.instance);
        free(u);
}

UnitActiveState unit_active_state(Unit *u) {
        assert(u);

        if (u->meta.load_state == UNIT_MERGED)
                return unit_active_state(unit_follow_merge(u));

        /* After a reload it might happen that a unit is not correctly
         * loaded but still has a process around. That's why we won't
         * shortcut failed loading to UNIT_INACTIVE_FAILED. */

        return UNIT_VTABLE(u)->active_state(u);
}

const char* unit_sub_state_to_string(Unit *u) {
        assert(u);

        return UNIT_VTABLE(u)->sub_state_to_string(u);
}

static void complete_move(Set **s, Set **other) {
        assert(s);
        assert(other);

        if (!*other)
                return;

        if (*s)
                set_move(*s, *other);
        else {
                *s = *other;
                *other = NULL;
        }
}

static void merge_names(Unit *u, Unit *other) {
        char *t;
        Iterator i;

        assert(u);
        assert(other);

        complete_move(&u->meta.names, &other->meta.names);

        set_free_free(other->meta.names);
        other->meta.names = NULL;
        other->meta.id = NULL;

        SET_FOREACH(t, u->meta.names, i)
                assert_se(hashmap_replace(u->meta.manager->units, t, u) == 0);
}

static void merge_dependencies(Unit *u, Unit *other, UnitDependency d) {
        Iterator i;
        Unit *back;
        int r;

        assert(u);
        assert(other);
        assert(d < _UNIT_DEPENDENCY_MAX);

        /* Fix backwards pointers */
        SET_FOREACH(back, other->meta.dependencies[d], i) {
                UnitDependency k;

                for (k = 0; k < _UNIT_DEPENDENCY_MAX; k++)
                        if ((r = set_remove_and_put(back->meta.dependencies[k], other, u)) < 0) {

                                if (r == -EEXIST)
                                        set_remove(back->meta.dependencies[k], other);
                                else
                                        assert(r == -ENOENT);
                        }
        }

        complete_move(&u->meta.dependencies[d], &other->meta.dependencies[d]);

        set_free(other->meta.dependencies[d]);
        other->meta.dependencies[d] = NULL;
}

int unit_merge(Unit *u, Unit *other) {
        UnitDependency d;

        assert(u);
        assert(other);
        assert(u->meta.manager == other->meta.manager);
        assert(u->meta.type != _UNIT_TYPE_INVALID);

        other = unit_follow_merge(other);

        if (other == u)
                return 0;

        if (u->meta.type != other->meta.type)
                return -EINVAL;

        if (!u->meta.instance != !other->meta.instance)
                return -EINVAL;

        if (other->meta.load_state != UNIT_STUB &&
            other->meta.load_state != UNIT_ERROR)
                return -EEXIST;

        if (other->meta.job)
                return -EEXIST;

        if (!UNIT_IS_INACTIVE_OR_FAILED(unit_active_state(other)))
                return -EEXIST;

        /* Merge names */
        merge_names(u, other);

        /* Merge dependencies */
        for (d = 0; d < _UNIT_DEPENDENCY_MAX; d++)
                merge_dependencies(u, other, d);

        other->meta.load_state = UNIT_MERGED;
        other->meta.merged_into = u;

        /* If there is still some data attached to the other node, we
         * don't need it anymore, and can free it. */
        if (other->meta.load_state != UNIT_STUB)
                if (UNIT_VTABLE(other)->done)
                        UNIT_VTABLE(other)->done(other);

        unit_add_to_dbus_queue(u);
        unit_add_to_cleanup_queue(other);

        return 0;
}

int unit_merge_by_name(Unit *u, const char *name) {
        Unit *other;
        int r;
        char *s = NULL;

        assert(u);
        assert(name);

        if (unit_name_is_template(name)) {
                if (!u->meta.instance)
                        return -EINVAL;

                if (!(s = unit_name_replace_instance(name, u->meta.instance)))
                        return -ENOMEM;

                name = s;
        }

        if (!(other = manager_get_unit(u->meta.manager, name)))
                r = unit_add_name(u, name);
        else
                r = unit_merge(u, other);

        free(s);
        return r;
}

Unit* unit_follow_merge(Unit *u) {
        assert(u);

        while (u->meta.load_state == UNIT_MERGED)
                assert_se(u = u->meta.merged_into);

        return u;
}

int unit_add_exec_dependencies(Unit *u, ExecContext *c) {
        int r;

        assert(u);
        assert(c);

        if (c->std_output != EXEC_OUTPUT_KMSG &&
            c->std_output != EXEC_OUTPUT_SYSLOG &&
            c->std_output != EXEC_OUTPUT_KMSG_AND_CONSOLE &&
            c->std_output != EXEC_OUTPUT_SYSLOG_AND_CONSOLE &&
            c->std_error != EXEC_OUTPUT_KMSG &&
            c->std_error != EXEC_OUTPUT_SYSLOG_AND_CONSOLE &&
            c->std_error != EXEC_OUTPUT_KMSG &&
            c->std_error != EXEC_OUTPUT_SYSLOG_AND_CONSOLE)
                return 0;

        /* If syslog or kernel logging is requested, make sure our own
         * logging daemon is run first. */

        if (u->meta.manager->running_as == MANAGER_SYSTEM)
                if ((r = unit_add_two_dependencies_by_name(u, UNIT_REQUIRES, UNIT_AFTER, SPECIAL_LOGGER_SOCKET, NULL, true)) < 0)
                        return r;

        return 0;
}

const char *unit_description(Unit *u) {
        assert(u);

        if (u->meta.description)
                return u->meta.description;

        return strna(u->meta.id);
}

void unit_dump(Unit *u, FILE *f, const char *prefix) {
        char *t;
        UnitDependency d;
        Iterator i;
        char *p2;
        const char *prefix2;
        CGroupBonding *b;
        char
                timestamp1[FORMAT_TIMESTAMP_MAX],
                timestamp2[FORMAT_TIMESTAMP_MAX],
                timestamp3[FORMAT_TIMESTAMP_MAX],
                timestamp4[FORMAT_TIMESTAMP_MAX],
                timespan[FORMAT_TIMESPAN_MAX];
        Unit *following;

        assert(u);
        assert(u->meta.type >= 0);

        if (!prefix)
                prefix = "";
        p2 = strappend(prefix, "\t");
        prefix2 = p2 ? p2 : prefix;

        fprintf(f,
                "%s-> Unit %s:\n"
                "%s\tDescription: %s\n"
                "%s\tInstance: %s\n"
                "%s\tUnit Load State: %s\n"
                "%s\tUnit Active State: %s\n"
                "%s\tInactive Exit Timestamp: %s\n"
                "%s\tActive Enter Timestamp: %s\n"
                "%s\tActive Exit Timestamp: %s\n"
                "%s\tInactive Enter Timestamp: %s\n"
                "%s\tGC Check Good: %s\n"
                "%s\tNeed Daemon Reload: %s\n",
                prefix, u->meta.id,
                prefix, unit_description(u),
                prefix, strna(u->meta.instance),
                prefix, unit_load_state_to_string(u->meta.load_state),
                prefix, unit_active_state_to_string(unit_active_state(u)),
                prefix, strna(format_timestamp(timestamp1, sizeof(timestamp1), u->meta.inactive_exit_timestamp.realtime)),
                prefix, strna(format_timestamp(timestamp2, sizeof(timestamp2), u->meta.active_enter_timestamp.realtime)),
                prefix, strna(format_timestamp(timestamp3, sizeof(timestamp3), u->meta.active_exit_timestamp.realtime)),
                prefix, strna(format_timestamp(timestamp4, sizeof(timestamp4), u->meta.inactive_enter_timestamp.realtime)),
                prefix, yes_no(unit_check_gc(u)),
                prefix, yes_no(unit_need_daemon_reload(u)));

        SET_FOREACH(t, u->meta.names, i)
                fprintf(f, "%s\tName: %s\n", prefix, t);

        if ((following = unit_following(u)))
                fprintf(f, "%s\tFollowing: %s\n", prefix, following->meta.id);

        if (u->meta.fragment_path)
                fprintf(f, "%s\tFragment Path: %s\n", prefix, u->meta.fragment_path);

        if (u->meta.job_timeout > 0)
                fprintf(f, "%s\tJob Timeout: %s\n", prefix, format_timespan(timespan, sizeof(timespan), u->meta.job_timeout));

        condition_dump_list(u->meta.conditions, f, prefix);

        if (dual_timestamp_is_set(&u->meta.condition_timestamp))
                fprintf(f,
                        "%s\tCondition Timestamp: %s\n"
                        "%s\tCondition Result: %s\n",
                        prefix, strna(format_timestamp(timestamp1, sizeof(timestamp1), u->meta.condition_timestamp.realtime)),
                        prefix, yes_no(u->meta.condition_result));

        for (d = 0; d < _UNIT_DEPENDENCY_MAX; d++) {
                Unit *other;

                SET_FOREACH(other, u->meta.dependencies[d], i)
                        fprintf(f, "%s\t%s: %s\n", prefix, unit_dependency_to_string(d), other->meta.id);
        }

        if (u->meta.load_state == UNIT_LOADED) {
                fprintf(f,
                        "%s\tStopWhenUnneeded: %s\n"
                        "%s\tRefuseManualStart: %s\n"
                        "%s\tRefuseManualStop: %s\n"
                        "%s\tDefaultDependencies: %s\n"
                        "%s\tOnFailureIsolate: %s\n"
                        "%s\tIgnoreOnIsolate: %s\n"
                        "%s\tIgnoreOnSnapshot: %s\n",
                        prefix, yes_no(u->meta.stop_when_unneeded),
                        prefix, yes_no(u->meta.refuse_manual_start),
                        prefix, yes_no(u->meta.refuse_manual_stop),
                        prefix, yes_no(u->meta.default_dependencies),
                        prefix, yes_no(u->meta.on_failure_isolate),
                        prefix, yes_no(u->meta.ignore_on_isolate),
                        prefix, yes_no(u->meta.ignore_on_snapshot));

                LIST_FOREACH(by_unit, b, u->meta.cgroup_bondings)
                        fprintf(f, "%s\tControlGroup: %s:%s\n",
                                prefix, b->controller, b->path);

                if (UNIT_VTABLE(u)->dump)
                        UNIT_VTABLE(u)->dump(u, f, prefix2);

        } else if (u->meta.load_state == UNIT_MERGED)
                fprintf(f,
                        "%s\tMerged into: %s\n",
                        prefix, u->meta.merged_into->meta.id);
        else if (u->meta.load_state == UNIT_ERROR)
                fprintf(f, "%s\tLoad Error Code: %s\n", prefix, strerror(-u->meta.load_error));


        if (u->meta.job)
                job_dump(u->meta.job, f, prefix2);

        free(p2);
}

/* Common implementation for multiple backends */
int unit_load_fragment_and_dropin(Unit *u) {
        int r;

        assert(u);

        /* Load a .service file */
        if ((r = unit_load_fragment(u)) < 0)
                return r;

        if (u->meta.load_state == UNIT_STUB)
                return -ENOENT;

        /* Load drop-in directory data */
        if ((r = unit_load_dropin(unit_follow_merge(u))) < 0)
                return r;

        return 0;
}

/* Common implementation for multiple backends */
int unit_load_fragment_and_dropin_optional(Unit *u) {
        int r;

        assert(u);

        /* Same as unit_load_fragment_and_dropin(), but whether
         * something can be loaded or not doesn't matter. */

        /* Load a .service file */
        if ((r = unit_load_fragment(u)) < 0)
                return r;

        if (u->meta.load_state == UNIT_STUB)
                u->meta.load_state = UNIT_LOADED;

        /* Load drop-in directory data */
        if ((r = unit_load_dropin(unit_follow_merge(u))) < 0)
                return r;

        return 0;
}

int unit_add_default_target_dependency(Unit *u, Unit *target) {
        assert(u);
        assert(target);

        if (target->meta.type != UNIT_TARGET)
                return 0;

        /* Only add the dependency if both units are loaded, so that
         * that loop check below is reliable */
        if (u->meta.load_state != UNIT_LOADED ||
            target->meta.load_state != UNIT_LOADED)
                return 0;

        /* If either side wants no automatic dependencies, then let's
         * skip this */
        if (!u->meta.default_dependencies ||
            target->meta.default_dependencies)
                return 0;

        /* Don't create loops */
        if (set_get(target->meta.dependencies[UNIT_BEFORE], u))
                return 0;

        return unit_add_dependency(target, UNIT_AFTER, u, true);
}

static int unit_add_default_dependencies(Unit *u) {
        static const UnitDependency deps[] = {
                UNIT_REQUIRED_BY,
                UNIT_REQUIRED_BY_OVERRIDABLE,
                UNIT_WANTED_BY,
                UNIT_BOUND_BY
        };

        Unit *target;
        Iterator i;
        int r;
        unsigned k;

        assert(u);

        for (k = 0; k < ELEMENTSOF(deps); k++)
                SET_FOREACH(target, u->meta.dependencies[deps[k]], i)
                        if ((r = unit_add_default_target_dependency(u, target)) < 0)
                                return r;

        return 0;
}

int unit_load(Unit *u) {
        int r;

        assert(u);

        if (u->meta.in_load_queue) {
                LIST_REMOVE(Meta, load_queue, u->meta.manager->load_queue, &u->meta);
                u->meta.in_load_queue = false;
        }

        if (u->meta.type == _UNIT_TYPE_INVALID)
                return -EINVAL;

        if (u->meta.load_state != UNIT_STUB)
                return 0;

        if (UNIT_VTABLE(u)->load)
                if ((r = UNIT_VTABLE(u)->load(u)) < 0)
                        goto fail;

        if (u->meta.load_state == UNIT_STUB) {
                r = -ENOENT;
                goto fail;
        }

        if (u->meta.load_state == UNIT_LOADED &&
            u->meta.default_dependencies)
                if ((r = unit_add_default_dependencies(u)) < 0)
                        goto fail;

        if (u->meta.on_failure_isolate &&
            set_size(u->meta.dependencies[UNIT_ON_FAILURE]) > 1) {

                log_error("More than one OnFailure= dependencies specified for %s but OnFailureIsolate= enabled. Refusing.",
                          u->meta.id);

                r = -EINVAL;
                goto fail;
        }

        assert((u->meta.load_state != UNIT_MERGED) == !u->meta.merged_into);

        unit_add_to_dbus_queue(unit_follow_merge(u));
        unit_add_to_gc_queue(u);

        return 0;

fail:
        u->meta.load_state = UNIT_ERROR;
        u->meta.load_error = r;
        unit_add_to_dbus_queue(u);

        log_debug("Failed to load configuration for %s: %s", u->meta.id, strerror(-r));

        return r;
}

bool unit_condition_test(Unit *u) {
        assert(u);

        dual_timestamp_get(&u->meta.condition_timestamp);
        u->meta.condition_result = condition_test_list(u->meta.conditions);

        return u->meta.condition_result;
}

/* Errors:
 *         -EBADR:     This unit type does not support starting.
 *         -EALREADY:  Unit is already started.
 *         -EAGAIN:    An operation is already in progress. Retry later.
 *         -ECANCELED: Too many requests for now.
 */
int unit_start(Unit *u) {
        UnitActiveState state;
        Unit *following;

        assert(u);

        if (u->meta.load_state != UNIT_LOADED)
                return -EINVAL;

        /* If this is already (being) started, then this will
         * succeed. Note that this will even succeed if this unit is
         * not startable by the user. This is relied on to detect when
         * we need to wait for units and when waiting is finished. */
        state = unit_active_state(u);
        if (UNIT_IS_ACTIVE_OR_RELOADING(state))
                return -EALREADY;

        /* If the conditions failed, don't do anything at all */
        if (!unit_condition_test(u)) {
                log_debug("Starting of %s requested but condition failed. Ignoring.", u->meta.id);
                return -EALREADY;
        }

        /* Forward to the main object, if we aren't it. */
        if ((following = unit_following(u))) {
                log_debug("Redirecting start request from %s to %s.", u->meta.id, following->meta.id);
                return unit_start(following);
        }

        /* If it is stopped, but we cannot start it, then fail */
        if (!UNIT_VTABLE(u)->start)
                return -EBADR;

        /* We don't suppress calls to ->start() here when we are
         * already starting, to allow this request to be used as a
         * "hurry up" call, for example when the unit is in some "auto
         * restart" state where it waits for a holdoff timer to elapse
         * before it will start again. */

        unit_add_to_dbus_queue(u);

        unit_status_printf(u, "Starting %s...\n", unit_description(u));
        return UNIT_VTABLE(u)->start(u);
}

bool unit_can_start(Unit *u) {
        assert(u);

        return !!UNIT_VTABLE(u)->start;
}

bool unit_can_isolate(Unit *u) {
        assert(u);

        return unit_can_start(u) &&
                u->meta.allow_isolate;
}

/* Errors:
 *         -EBADR:    This unit type does not support stopping.
 *         -EALREADY: Unit is already stopped.
 *         -EAGAIN:   An operation is already in progress. Retry later.
 */
int unit_stop(Unit *u) {
        UnitActiveState state;
        Unit *following;

        assert(u);

        state = unit_active_state(u);
        if (UNIT_IS_INACTIVE_OR_FAILED(state))
                return -EALREADY;

        if ((following = unit_following(u))) {
                log_debug("Redirecting stop request from %s to %s.", u->meta.id, following->meta.id);
                return unit_stop(following);
        }

        if (!UNIT_VTABLE(u)->stop)
                return -EBADR;

        unit_add_to_dbus_queue(u);

        unit_status_printf(u, "Stopping %s...\n", unit_description(u));
        return UNIT_VTABLE(u)->stop(u);
}

/* Errors:
 *         -EBADR:    This unit type does not support reloading.
 *         -ENOEXEC:  Unit is not started.
 *         -EAGAIN:   An operation is already in progress. Retry later.
 */
int unit_reload(Unit *u) {
        UnitActiveState state;
        Unit *following;

        assert(u);

        if (u->meta.load_state != UNIT_LOADED)
                return -EINVAL;

        if (!unit_can_reload(u))
                return -EBADR;

        state = unit_active_state(u);
        if (state == UNIT_RELOADING)
                return -EALREADY;

        if (state != UNIT_ACTIVE)
                return -ENOEXEC;

        if ((following = unit_following(u))) {
                log_debug("Redirecting reload request from %s to %s.", u->meta.id, following->meta.id);
                return unit_reload(following);
        }

        unit_add_to_dbus_queue(u);
        return UNIT_VTABLE(u)->reload(u);
}

bool unit_can_reload(Unit *u) {
        assert(u);

        if (!UNIT_VTABLE(u)->reload)
                return false;

        if (!UNIT_VTABLE(u)->can_reload)
                return true;

        return UNIT_VTABLE(u)->can_reload(u);
}

static void unit_check_unneeded(Unit *u) {
        Iterator i;
        Unit *other;

        assert(u);

        /* If this service shall be shut down when unneeded then do
         * so. */

        if (!u->meta.stop_when_unneeded)
                return;

        if (!UNIT_IS_ACTIVE_OR_ACTIVATING(unit_active_state(u)))
                return;

        SET_FOREACH(other, u->meta.dependencies[UNIT_REQUIRED_BY], i)
                if (!UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(other)))
                        return;

        SET_FOREACH(other, u->meta.dependencies[UNIT_REQUIRED_BY_OVERRIDABLE], i)
                if (!UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(other)))
                        return;

        SET_FOREACH(other, u->meta.dependencies[UNIT_WANTED_BY], i)
                if (!UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(other)))
                        return;

        SET_FOREACH(other, u->meta.dependencies[UNIT_BOUND_BY], i)
                if (!UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(other)))
                        return;

        log_info("Service %s is not needed anymore. Stopping.", u->meta.id);

        /* Ok, nobody needs us anymore. Sniff. Then let's commit suicide */
        manager_add_job(u->meta.manager, JOB_STOP, u, JOB_FAIL, true, NULL, NULL);
}

static void retroactively_start_dependencies(Unit *u) {
        Iterator i;
        Unit *other;

        assert(u);
        assert(UNIT_IS_ACTIVE_OR_ACTIVATING(unit_active_state(u)));

        SET_FOREACH(other, u->meta.dependencies[UNIT_REQUIRES], i)
                if (!set_get(u->meta.dependencies[UNIT_AFTER], other) &&
                    !UNIT_IS_ACTIVE_OR_ACTIVATING(unit_active_state(other)))
                        manager_add_job(u->meta.manager, JOB_START, other, JOB_REPLACE, true, NULL, NULL);

        SET_FOREACH(other, u->meta.dependencies[UNIT_BIND_TO], i)
                if (!set_get(u->meta.dependencies[UNIT_AFTER], other) &&
                    !UNIT_IS_ACTIVE_OR_ACTIVATING(unit_active_state(other)))
                        manager_add_job(u->meta.manager, JOB_START, other, JOB_REPLACE, true, NULL, NULL);

        SET_FOREACH(other, u->meta.dependencies[UNIT_REQUIRES_OVERRIDABLE], i)
                if (!set_get(u->meta.dependencies[UNIT_AFTER], other) &&
                    !UNIT_IS_ACTIVE_OR_ACTIVATING(unit_active_state(other)))
                        manager_add_job(u->meta.manager, JOB_START, other, JOB_FAIL, false, NULL, NULL);

        SET_FOREACH(other, u->meta.dependencies[UNIT_REQUISITE], i)
                if (!set_get(u->meta.dependencies[UNIT_AFTER], other) &&
                    !UNIT_IS_ACTIVE_OR_ACTIVATING(unit_active_state(other)))
                        manager_add_job(u->meta.manager, JOB_START, other, JOB_REPLACE, true, NULL, NULL);

        SET_FOREACH(other, u->meta.dependencies[UNIT_WANTS], i)
                if (!set_get(u->meta.dependencies[UNIT_AFTER], other) &&
                    !UNIT_IS_ACTIVE_OR_ACTIVATING(unit_active_state(other)))
                        manager_add_job(u->meta.manager, JOB_START, other, JOB_FAIL, false, NULL, NULL);

        SET_FOREACH(other, u->meta.dependencies[UNIT_CONFLICTS], i)
                if (!UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(other)))
                        manager_add_job(u->meta.manager, JOB_STOP, other, JOB_REPLACE, true, NULL, NULL);

        SET_FOREACH(other, u->meta.dependencies[UNIT_CONFLICTED_BY], i)
                if (!UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(other)))
                        manager_add_job(u->meta.manager, JOB_STOP, other, JOB_REPLACE, true, NULL, NULL);
}

static void retroactively_stop_dependencies(Unit *u) {
        Iterator i;
        Unit *other;

        assert(u);
        assert(UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(u)));

        /* Pull down units which are bound to us recursively if enabled */
        SET_FOREACH(other, u->meta.dependencies[UNIT_BOUND_BY], i)
                if (!UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(other)))
                        manager_add_job(u->meta.manager, JOB_STOP, other, JOB_REPLACE, true, NULL, NULL);

        /* Garbage collect services that might not be needed anymore, if enabled */
        SET_FOREACH(other, u->meta.dependencies[UNIT_REQUIRES], i)
                if (!UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(other)))
                        unit_check_unneeded(other);
        SET_FOREACH(other, u->meta.dependencies[UNIT_REQUIRES_OVERRIDABLE], i)
                if (!UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(other)))
                        unit_check_unneeded(other);
        SET_FOREACH(other, u->meta.dependencies[UNIT_WANTS], i)
                if (!UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(other)))
                        unit_check_unneeded(other);
        SET_FOREACH(other, u->meta.dependencies[UNIT_REQUISITE], i)
                if (!UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(other)))
                        unit_check_unneeded(other);
        SET_FOREACH(other, u->meta.dependencies[UNIT_REQUISITE_OVERRIDABLE], i)
                if (!UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(other)))
                        unit_check_unneeded(other);
        SET_FOREACH(other, u->meta.dependencies[UNIT_BIND_TO], i)
                if (!UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(other)))
                        unit_check_unneeded(other);
}

void unit_trigger_on_failure(Unit *u) {
        Unit *other;
        Iterator i;

        assert(u);

        if (set_size(u->meta.dependencies[UNIT_ON_FAILURE]) <= 0)
                return;

        log_info("Triggering OnFailure= dependencies of %s.", u->meta.id);

        SET_FOREACH(other, u->meta.dependencies[UNIT_ON_FAILURE], i) {
                int r;

                if ((r = manager_add_job(u->meta.manager, JOB_START, other, u->meta.on_failure_isolate ? JOB_ISOLATE : JOB_REPLACE, true, NULL, NULL)) < 0)
                        log_error("Failed to enqueue OnFailure= job: %s", strerror(-r));
        }
}

void unit_notify(Unit *u, UnitActiveState os, UnitActiveState ns, bool reload_success) {
        bool unexpected;

        assert(u);
        assert(os < _UNIT_ACTIVE_STATE_MAX);
        assert(ns < _UNIT_ACTIVE_STATE_MAX);

        /* Note that this is called for all low-level state changes,
         * even if they might map to the same high-level
         * UnitActiveState! That means that ns == os is OK an expected
         * behaviour here. For example: if a mount point is remounted
         * this function will be called too! */

        if (u->meta.manager->n_deserializing <= 0) {
                dual_timestamp ts;

                dual_timestamp_get(&ts);

                if (UNIT_IS_INACTIVE_OR_FAILED(os) && !UNIT_IS_INACTIVE_OR_FAILED(ns))
                        u->meta.inactive_exit_timestamp = ts;
                else if (!UNIT_IS_INACTIVE_OR_FAILED(os) && UNIT_IS_INACTIVE_OR_FAILED(ns))
                        u->meta.inactive_enter_timestamp = ts;

                if (!UNIT_IS_ACTIVE_OR_RELOADING(os) && UNIT_IS_ACTIVE_OR_RELOADING(ns))
                        u->meta.active_enter_timestamp = ts;
                else if (UNIT_IS_ACTIVE_OR_RELOADING(os) && !UNIT_IS_ACTIVE_OR_RELOADING(ns))
                        u->meta.active_exit_timestamp = ts;

                timer_unit_notify(u, ns);
                path_unit_notify(u, ns);
        }

        if (UNIT_IS_INACTIVE_OR_FAILED(ns))
                cgroup_bonding_trim_list(u->meta.cgroup_bondings, true);

        if (u->meta.job) {
                unexpected = false;

                if (u->meta.job->state == JOB_WAITING)

                        /* So we reached a different state for this
                         * job. Let's see if we can run it now if it
                         * failed previously due to EAGAIN. */
                        job_add_to_run_queue(u->meta.job);

                /* Let's check whether this state change constitutes a
                 * finished job, or maybe contradicts a running job and
                 * hence needs to invalidate jobs. */

                switch (u->meta.job->type) {

                case JOB_START:
                case JOB_VERIFY_ACTIVE:

                        if (UNIT_IS_ACTIVE_OR_RELOADING(ns))
                                job_finish_and_invalidate(u->meta.job, JOB_DONE);
                        else if (u->meta.job->state == JOB_RUNNING && ns != UNIT_ACTIVATING) {
                                unexpected = true;

                                if (UNIT_IS_INACTIVE_OR_FAILED(ns))
                                        job_finish_and_invalidate(u->meta.job, ns == UNIT_FAILED ? JOB_FAILED : JOB_DONE);
                        }

                        break;

                case JOB_RELOAD:
                case JOB_RELOAD_OR_START:

                        if (u->meta.job->state == JOB_RUNNING) {
                                if (ns == UNIT_ACTIVE)
                                        job_finish_and_invalidate(u->meta.job, reload_success ? JOB_DONE : JOB_FAILED);
                                else if (ns != UNIT_ACTIVATING && ns != UNIT_RELOADING) {
                                        unexpected = true;

                                        if (UNIT_IS_INACTIVE_OR_FAILED(ns))
                                                job_finish_and_invalidate(u->meta.job, ns == UNIT_FAILED ? JOB_FAILED : JOB_DONE);
                                }
                        }

                        break;

                case JOB_STOP:
                case JOB_RESTART:
                case JOB_TRY_RESTART:

                        if (UNIT_IS_INACTIVE_OR_FAILED(ns))
                                job_finish_and_invalidate(u->meta.job, JOB_DONE);
                        else if (u->meta.job->state == JOB_RUNNING && ns != UNIT_DEACTIVATING) {
                                unexpected = true;
                                job_finish_and_invalidate(u->meta.job, JOB_FAILED);
                        }

                        break;

                default:
                        assert_not_reached("Job type unknown");
                }

        } else
                unexpected = true;

        if (u->meta.manager->n_deserializing <= 0) {

                /* If this state change happened without being
                 * requested by a job, then let's retroactively start
                 * or stop dependencies. We skip that step when
                 * deserializing, since we don't want to create any
                 * additional jobs just because something is already
                 * activated. */

                if (unexpected) {
                        if (UNIT_IS_INACTIVE_OR_FAILED(os) && UNIT_IS_ACTIVE_OR_ACTIVATING(ns))
                                retroactively_start_dependencies(u);
                        else if (UNIT_IS_ACTIVE_OR_ACTIVATING(os) && UNIT_IS_INACTIVE_OR_DEACTIVATING(ns))
                                retroactively_stop_dependencies(u);
                }

                if (ns != os && ns == UNIT_FAILED) {
                        log_notice("Unit %s entered failed state.", u->meta.id);
                        unit_trigger_on_failure(u);
                }
        }

        /* Some names are special */
        if (UNIT_IS_ACTIVE_OR_RELOADING(ns)) {

                if (unit_has_name(u, SPECIAL_DBUS_SERVICE))
                        /* The bus just might have become available,
                         * hence try to connect to it, if we aren't
                         * yet connected. */
                        bus_init(u->meta.manager, true);

                if (u->meta.type == UNIT_SERVICE &&
                    !UNIT_IS_ACTIVE_OR_RELOADING(os) &&
                    u->meta.manager->n_deserializing <= 0) {
                        /* Write audit record if we have just finished starting up */
                        manager_send_unit_audit(u->meta.manager, u, AUDIT_SERVICE_START, true);
                        u->meta.in_audit = true;
                }

                if (!UNIT_IS_ACTIVE_OR_RELOADING(os))
                        manager_send_unit_plymouth(u->meta.manager, u);

        } else {

                /* We don't care about D-Bus here, since we'll get an
                 * asynchronous notification for it anyway. */

                if (u->meta.type == UNIT_SERVICE &&
                    UNIT_IS_INACTIVE_OR_FAILED(ns) &&
                    !UNIT_IS_INACTIVE_OR_FAILED(os) &&
                    u->meta.manager->n_deserializing <= 0) {

                        /* Hmm, if there was no start record written
                         * write it now, so that we always have a nice
                         * pair */
                        if (!u->meta.in_audit) {
                                manager_send_unit_audit(u->meta.manager, u, AUDIT_SERVICE_START, ns == UNIT_INACTIVE);

                                if (ns == UNIT_INACTIVE)
                                        manager_send_unit_audit(u->meta.manager, u, AUDIT_SERVICE_STOP, true);
                        } else
                                /* Write audit record if we have just finished shutting down */
                                manager_send_unit_audit(u->meta.manager, u, AUDIT_SERVICE_STOP, ns == UNIT_INACTIVE);

                        u->meta.in_audit = false;
                }
        }

        manager_recheck_syslog(u->meta.manager);

        /* Maybe we finished startup and are now ready for being
         * stopped because unneeded? */
        unit_check_unneeded(u);

        unit_add_to_dbus_queue(u);
        unit_add_to_gc_queue(u);
}

int unit_watch_fd(Unit *u, int fd, uint32_t events, Watch *w) {
        struct epoll_event ev;

        assert(u);
        assert(fd >= 0);
        assert(w);
        assert(w->type == WATCH_INVALID || (w->type == WATCH_FD && w->fd == fd && w->data.unit == u));

        zero(ev);
        ev.data.ptr = w;
        ev.events = events;

        if (epoll_ctl(u->meta.manager->epoll_fd,
                      w->type == WATCH_INVALID ? EPOLL_CTL_ADD : EPOLL_CTL_MOD,
                      fd,
                      &ev) < 0)
                return -errno;

        w->fd = fd;
        w->type = WATCH_FD;
        w->data.unit = u;

        return 0;
}

void unit_unwatch_fd(Unit *u, Watch *w) {
        assert(u);
        assert(w);

        if (w->type == WATCH_INVALID)
                return;

        assert(w->type == WATCH_FD);
        assert(w->data.unit == u);
        assert_se(epoll_ctl(u->meta.manager->epoll_fd, EPOLL_CTL_DEL, w->fd, NULL) >= 0);

        w->fd = -1;
        w->type = WATCH_INVALID;
        w->data.unit = NULL;
}

int unit_watch_pid(Unit *u, pid_t pid) {
        assert(u);
        assert(pid >= 1);

        /* Watch a specific PID. We only support one unit watching
         * each PID for now. */

        return hashmap_put(u->meta.manager->watch_pids, LONG_TO_PTR(pid), u);
}

void unit_unwatch_pid(Unit *u, pid_t pid) {
        assert(u);
        assert(pid >= 1);

        hashmap_remove_value(u->meta.manager->watch_pids, LONG_TO_PTR(pid), u);
}

int unit_watch_timer(Unit *u, usec_t delay, Watch *w) {
        struct itimerspec its;
        int flags, fd;
        bool ours;

        assert(u);
        assert(w);
        assert(w->type == WATCH_INVALID || (w->type == WATCH_UNIT_TIMER && w->data.unit == u));

        /* This will try to reuse the old timer if there is one */

        if (w->type == WATCH_UNIT_TIMER) {
                assert(w->data.unit == u);
                assert(w->fd >= 0);

                ours = false;
                fd = w->fd;
        } else if (w->type == WATCH_INVALID) {

                ours = true;
                if ((fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC)) < 0)
                        return -errno;
        } else
                assert_not_reached("Invalid watch type");

        zero(its);

        if (delay <= 0) {
                /* Set absolute time in the past, but not 0, since we
                 * don't want to disarm the timer */
                its.it_value.tv_sec = 0;
                its.it_value.tv_nsec = 1;

                flags = TFD_TIMER_ABSTIME;
        } else {
                timespec_store(&its.it_value, delay);
                flags = 0;
        }

        /* This will also flush the elapse counter */
        if (timerfd_settime(fd, flags, &its, NULL) < 0)
                goto fail;

        if (w->type == WATCH_INVALID) {
                struct epoll_event ev;

                zero(ev);
                ev.data.ptr = w;
                ev.events = EPOLLIN;

                if (epoll_ctl(u->meta.manager->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
                        goto fail;
        }

        w->type = WATCH_UNIT_TIMER;
        w->fd = fd;
        w->data.unit = u;

        return 0;

fail:
        if (ours)
                close_nointr_nofail(fd);

        return -errno;
}

void unit_unwatch_timer(Unit *u, Watch *w) {
        assert(u);
        assert(w);

        if (w->type == WATCH_INVALID)
                return;

        assert(w->type == WATCH_UNIT_TIMER);
        assert(w->data.unit == u);
        assert(w->fd >= 0);

        assert_se(epoll_ctl(u->meta.manager->epoll_fd, EPOLL_CTL_DEL, w->fd, NULL) >= 0);
        close_nointr_nofail(w->fd);

        w->fd = -1;
        w->type = WATCH_INVALID;
        w->data.unit = NULL;
}

bool unit_job_is_applicable(Unit *u, JobType j) {
        assert(u);
        assert(j >= 0 && j < _JOB_TYPE_MAX);

        switch (j) {

        case JOB_VERIFY_ACTIVE:
        case JOB_START:
        case JOB_STOP:
                return true;

        case JOB_RESTART:
        case JOB_TRY_RESTART:
                return unit_can_start(u);

        case JOB_RELOAD:
                return unit_can_reload(u);

        case JOB_RELOAD_OR_START:
                return unit_can_reload(u) && unit_can_start(u);

        default:
                assert_not_reached("Invalid job type");
        }
}

int unit_add_dependency(Unit *u, UnitDependency d, Unit *other, bool add_reference) {

        static const UnitDependency inverse_table[_UNIT_DEPENDENCY_MAX] = {
                [UNIT_REQUIRES] = UNIT_REQUIRED_BY,
                [UNIT_REQUIRES_OVERRIDABLE] = UNIT_REQUIRED_BY_OVERRIDABLE,
                [UNIT_WANTS] = UNIT_WANTED_BY,
                [UNIT_REQUISITE] = UNIT_REQUIRED_BY,
                [UNIT_REQUISITE_OVERRIDABLE] = UNIT_REQUIRED_BY_OVERRIDABLE,
                [UNIT_BIND_TO] = UNIT_BOUND_BY,
                [UNIT_REQUIRED_BY] = _UNIT_DEPENDENCY_INVALID,
                [UNIT_REQUIRED_BY_OVERRIDABLE] = _UNIT_DEPENDENCY_INVALID,
                [UNIT_WANTED_BY] = _UNIT_DEPENDENCY_INVALID,
                [UNIT_BOUND_BY] = UNIT_BIND_TO,
                [UNIT_CONFLICTS] = UNIT_CONFLICTED_BY,
                [UNIT_CONFLICTED_BY] = UNIT_CONFLICTS,
                [UNIT_BEFORE] = UNIT_AFTER,
                [UNIT_AFTER] = UNIT_BEFORE,
                [UNIT_ON_FAILURE] = _UNIT_DEPENDENCY_INVALID,
                [UNIT_REFERENCES] = UNIT_REFERENCED_BY,
                [UNIT_REFERENCED_BY] = UNIT_REFERENCES
        };
        int r, q = 0, v = 0, w = 0;

        assert(u);
        assert(d >= 0 && d < _UNIT_DEPENDENCY_MAX);
        assert(other);

        u = unit_follow_merge(u);
        other = unit_follow_merge(other);

        /* We won't allow dependencies on ourselves. We will not
         * consider them an error however. */
        if (u == other)
                return 0;

        if ((r = set_ensure_allocated(&u->meta.dependencies[d], trivial_hash_func, trivial_compare_func)) < 0)
                return r;

        if (inverse_table[d] != _UNIT_DEPENDENCY_INVALID)
                if ((r = set_ensure_allocated(&other->meta.dependencies[inverse_table[d]], trivial_hash_func, trivial_compare_func)) < 0)
                        return r;

        if (add_reference)
                if ((r = set_ensure_allocated(&u->meta.dependencies[UNIT_REFERENCES], trivial_hash_func, trivial_compare_func)) < 0 ||
                    (r = set_ensure_allocated(&other->meta.dependencies[UNIT_REFERENCED_BY], trivial_hash_func, trivial_compare_func)) < 0)
                        return r;

        if ((q = set_put(u->meta.dependencies[d], other)) < 0)
                return q;

        if (inverse_table[d] != _UNIT_DEPENDENCY_INVALID)
                if ((v = set_put(other->meta.dependencies[inverse_table[d]], u)) < 0) {
                        r = v;
                        goto fail;
                }

        if (add_reference) {
                if ((w = set_put(u->meta.dependencies[UNIT_REFERENCES], other)) < 0) {
                        r = w;
                        goto fail;
                }

                if ((r = set_put(other->meta.dependencies[UNIT_REFERENCED_BY], u)) < 0)
                        goto fail;
        }

        unit_add_to_dbus_queue(u);
        return 0;

fail:
        if (q > 0)
                set_remove(u->meta.dependencies[d], other);

        if (v > 0)
                set_remove(other->meta.dependencies[inverse_table[d]], u);

        if (w > 0)
                set_remove(u->meta.dependencies[UNIT_REFERENCES], other);

        return r;
}

int unit_add_two_dependencies(Unit *u, UnitDependency d, UnitDependency e, Unit *other, bool add_reference) {
        int r;

        assert(u);

        if ((r = unit_add_dependency(u, d, other, add_reference)) < 0)
                return r;

        if ((r = unit_add_dependency(u, e, other, add_reference)) < 0)
                return r;

        return 0;
}

static const char *resolve_template(Unit *u, const char *name, const char*path, char **p) {
        char *s;

        assert(u);
        assert(name || path);

        if (!name)
                name = file_name_from_path(path);

        if (!unit_name_is_template(name)) {
                *p = NULL;
                return name;
        }

        if (u->meta.instance)
                s = unit_name_replace_instance(name, u->meta.instance);
        else {
                char *i;

                if (!(i = unit_name_to_prefix(u->meta.id)))
                        return NULL;

                s = unit_name_replace_instance(name, i);
                free(i);
        }

        if (!s)
                return NULL;

        *p = s;
        return s;
}

int unit_add_dependency_by_name(Unit *u, UnitDependency d, const char *name, const char *path, bool add_reference) {
        Unit *other;
        int r;
        char *s;

        assert(u);
        assert(name || path);

        if (!(name = resolve_template(u, name, path, &s)))
                return -ENOMEM;

        if ((r = manager_load_unit(u->meta.manager, name, path, NULL, &other)) < 0)
                goto finish;

        r = unit_add_dependency(u, d, other, add_reference);

finish:
        free(s);
        return r;
}

int unit_add_two_dependencies_by_name(Unit *u, UnitDependency d, UnitDependency e, const char *name, const char *path, bool add_reference) {
        Unit *other;
        int r;
        char *s;

        assert(u);
        assert(name || path);

        if (!(name = resolve_template(u, name, path, &s)))
                return -ENOMEM;

        if ((r = manager_load_unit(u->meta.manager, name, path, NULL, &other)) < 0)
                goto finish;

        r = unit_add_two_dependencies(u, d, e, other, add_reference);

finish:
        free(s);
        return r;
}

int unit_add_dependency_by_name_inverse(Unit *u, UnitDependency d, const char *name, const char *path, bool add_reference) {
        Unit *other;
        int r;
        char *s;

        assert(u);
        assert(name || path);

        if (!(name = resolve_template(u, name, path, &s)))
                return -ENOMEM;

        if ((r = manager_load_unit(u->meta.manager, name, path, NULL, &other)) < 0)
                goto finish;

        r = unit_add_dependency(other, d, u, add_reference);

finish:
        free(s);
        return r;
}

int unit_add_two_dependencies_by_name_inverse(Unit *u, UnitDependency d, UnitDependency e, const char *name, const char *path, bool add_reference) {
        Unit *other;
        int r;
        char *s;

        assert(u);
        assert(name || path);

        if (!(name = resolve_template(u, name, path, &s)))
                return -ENOMEM;

        if ((r = manager_load_unit(u->meta.manager, name, path, NULL, &other)) < 0)
                goto finish;

        if ((r = unit_add_two_dependencies(other, d, e, u, add_reference)) < 0)
                goto finish;

finish:
        free(s);
        return r;
}

int set_unit_path(const char *p) {
        char *cwd, *c;
        int r;

        /* This is mostly for debug purposes */

        if (path_is_absolute(p)) {
                if (!(c = strdup(p)))
                        return -ENOMEM;
        } else {
                if (!(cwd = get_current_dir_name()))
                        return -errno;

                r = asprintf(&c, "%s/%s", cwd, p);
                free(cwd);

                if (r < 0)
                        return -ENOMEM;
        }

        if (setenv("SYSTEMD_UNIT_PATH", c, 0) < 0) {
                r = -errno;
                free(c);
                return r;
        }

        return 0;
}

char *unit_dbus_path(Unit *u) {
        char *p, *e;

        assert(u);

        if (!u->meta.id)
                return NULL;

        if (!(e = bus_path_escape(u->meta.id)))
                return NULL;

        p = strappend("/org/freedesktop/systemd1/unit/", e);
        free(e);

        return p;
}

int unit_add_cgroup(Unit *u, CGroupBonding *b) {
        int r;

        assert(u);
        assert(b);

        assert(b->path);

        if (!b->controller)
                if (!(b->controller = strdup(SYSTEMD_CGROUP_CONTROLLER)))
                        return -ENOMEM;

        /* Ensure this hasn't been added yet */
        assert(!b->unit);

        if (streq(b->controller, SYSTEMD_CGROUP_CONTROLLER)) {
                CGroupBonding *l;

                l = hashmap_get(u->meta.manager->cgroup_bondings, b->path);
                LIST_PREPEND(CGroupBonding, by_path, l, b);

                if ((r = hashmap_replace(u->meta.manager->cgroup_bondings, b->path, l)) < 0) {
                        LIST_REMOVE(CGroupBonding, by_path, l, b);
                        return r;
                }
        }

        LIST_PREPEND(CGroupBonding, by_unit, u->meta.cgroup_bondings, b);
        b->unit = u;

        return 0;
}

static char *default_cgroup_path(Unit *u) {
        char *p;
        int r;

        assert(u);

        if (u->meta.instance) {
                char *t;

                if (!(t = unit_name_template(u->meta.id)))
                        return NULL;

                r = asprintf(&p, "%s/%s/%s", u->meta.manager->cgroup_hierarchy, t, u->meta.instance);
                free(t);
        } else
                r = asprintf(&p, "%s/%s", u->meta.manager->cgroup_hierarchy, u->meta.id);

        return r < 0 ? NULL : p;
}

int unit_add_cgroup_from_text(Unit *u, const char *name) {
        char *controller = NULL, *path = NULL;
        CGroupBonding *b = NULL;
        int r;

        assert(u);
        assert(name);

        if ((r = cg_split_spec(name, &controller, &path)) < 0)
                return r;

        if (!path)
                path = default_cgroup_path(u);

        if (!controller)
                controller = strdup(SYSTEMD_CGROUP_CONTROLLER);

        if (!path || !controller) {
                free(path);
                free(controller);

                return -ENOMEM;
        }

        if (cgroup_bonding_find_list(u->meta.cgroup_bondings, controller)) {
                r = -EEXIST;
                goto fail;
        }

        if (!(b = new0(CGroupBonding, 1))) {
                r = -ENOMEM;
                goto fail;
        }

        b->controller = controller;
        b->path = path;
        b->ours = false;

        if ((r = unit_add_cgroup(u, b)) < 0)
                goto fail;

        return 0;

fail:
        free(path);
        free(controller);
        free(b);

        return r;
}

static int unit_add_one_default_cgroup(Unit *u, const char *controller) {
        CGroupBonding *b = NULL;
        int r = -ENOMEM;

        assert(u);

        if (!controller)
                controller = SYSTEMD_CGROUP_CONTROLLER;

        if (cgroup_bonding_find_list(u->meta.cgroup_bondings, controller))
                return 0;

        if (!(b = new0(CGroupBonding, 1)))
                return -ENOMEM;

        if (!(b->controller = strdup(controller)))
                goto fail;

        if (!(b->path = default_cgroup_path(u)))
                goto fail;

        b->ours = true;
        b->essential = streq(controller, SYSTEMD_CGROUP_CONTROLLER);

        if ((r = unit_add_cgroup(u, b)) < 0)
                goto fail;

        return 0;

fail:
        free(b->path);
        free(b->controller);
        free(b);

        return r;
}

int unit_add_default_cgroups(Unit *u) {
        char **c;
        int r;
        assert(u);

        /* Adds in the default cgroups, if they weren't specified
         * otherwise. */

        if (!u->meta.manager->cgroup_hierarchy)
                return 0;

        if ((r = unit_add_one_default_cgroup(u, NULL)) < 0)
                return r;

        STRV_FOREACH(c, u->meta.manager->default_controllers)
                if ((r = unit_add_one_default_cgroup(u, *c)) < 0)
                        return r;

        return 0;
}

CGroupBonding* unit_get_default_cgroup(Unit *u) {
        assert(u);

        return cgroup_bonding_find_list(u->meta.cgroup_bondings, SYSTEMD_CGROUP_CONTROLLER);
}

int unit_load_related_unit(Unit *u, const char *type, Unit **_found) {
        char *t;
        int r;

        assert(u);
        assert(type);
        assert(_found);

        if (!(t = unit_name_change_suffix(u->meta.id, type)))
                return -ENOMEM;

        assert(!unit_has_name(u, t));

        r = manager_load_unit(u->meta.manager, t, NULL, NULL, _found);
        free(t);

        assert(r < 0 || *_found != u);

        return r;
}

int unit_get_related_unit(Unit *u, const char *type, Unit **_found) {
        Unit *found;
        char *t;

        assert(u);
        assert(type);
        assert(_found);

        if (!(t = unit_name_change_suffix(u->meta.id, type)))
                return -ENOMEM;

        assert(!unit_has_name(u, t));

        found = manager_get_unit(u->meta.manager, t);
        free(t);

        if (!found)
                return -ENOENT;

        *_found = found;
        return 0;
}

static char *specifier_prefix_and_instance(char specifier, void *data, void *userdata) {
        Unit *u = userdata;
        assert(u);

        return unit_name_to_prefix_and_instance(u->meta.id);
}

static char *specifier_prefix(char specifier, void *data, void *userdata) {
        Unit *u = userdata;
        assert(u);

        return unit_name_to_prefix(u->meta.id);
}

static char *specifier_prefix_unescaped(char specifier, void *data, void *userdata) {
        Unit *u = userdata;
        char *p, *r;

        assert(u);

        if (!(p = unit_name_to_prefix(u->meta.id)))
                return NULL;

        r = unit_name_unescape(p);
        free(p);

        return r;
}

static char *specifier_instance_unescaped(char specifier, void *data, void *userdata) {
        Unit *u = userdata;
        assert(u);

        if (u->meta.instance)
                return unit_name_unescape(u->meta.instance);

        return strdup("");
}

static char *specifier_filename(char specifier, void *data, void *userdata) {
        Unit *u = userdata;
        assert(u);

        if (u->meta.instance)
                return unit_name_path_unescape(u->meta.instance);

        return unit_name_to_path(u->meta.instance);
}

char *unit_name_printf(Unit *u, const char* format) {

        /*
         * This will use the passed string as format string and
         * replace the following specifiers:
         *
         * %n: the full id of the unit                 (foo@bar.waldo)
         * %N: the id of the unit without the suffix   (foo@bar)
         * %p: the prefix                              (foo)
         * %i: the instance                            (bar)
         */

        const Specifier table[] = {
                { 'n', specifier_string,              u->meta.id },
                { 'N', specifier_prefix_and_instance, NULL },
                { 'p', specifier_prefix,              NULL },
                { 'i', specifier_string,              u->meta.instance },
                { 0, NULL, NULL }
        };

        assert(u);
        assert(format);

        return specifier_printf(format, table, u);
}

char *unit_full_printf(Unit *u, const char *format) {

        /* This is similar to unit_name_printf() but also supports
         * unescaping */

        const Specifier table[] = {
                { 'n', specifier_string,              u->meta.id },
                { 'N', specifier_prefix_and_instance, NULL },
                { 'p', specifier_prefix,              NULL },
                { 'P', specifier_prefix_unescaped,    NULL },
                { 'i', specifier_string,              u->meta.instance },
                { 'I', specifier_instance_unescaped,  NULL },
                { 'f', specifier_filename,            NULL },
                { 0, NULL, NULL }
        };

        assert(u);
        assert(format);

        return specifier_printf(format, table, u);
}

char **unit_full_printf_strv(Unit *u, char **l) {
        size_t n;
        char **r, **i, **j;

        /* Applies unit_full_printf to every entry in l */

        assert(u);

        n = strv_length(l);
        if (!(r = new(char*, n+1)))
                return NULL;

        for (i = l, j = r; *i; i++, j++)
                if (!(*j = unit_full_printf(u, *i)))
                        goto fail;

        *j = NULL;
        return r;

fail:
        for (j--; j >= r; j--)
                free(*j);

        free(r);

        return NULL;
}

int unit_watch_bus_name(Unit *u, const char *name) {
        assert(u);
        assert(name);

        /* Watch a specific name on the bus. We only support one unit
         * watching each name for now. */

        return hashmap_put(u->meta.manager->watch_bus, name, u);
}

void unit_unwatch_bus_name(Unit *u, const char *name) {
        assert(u);
        assert(name);

        hashmap_remove_value(u->meta.manager->watch_bus, name, u);
}

bool unit_can_serialize(Unit *u) {
        assert(u);

        return UNIT_VTABLE(u)->serialize && UNIT_VTABLE(u)->deserialize_item;
}

int unit_serialize(Unit *u, FILE *f, FDSet *fds) {
        int r;

        assert(u);
        assert(f);
        assert(fds);

        if (!unit_can_serialize(u))
                return 0;

        if ((r = UNIT_VTABLE(u)->serialize(u, f, fds)) < 0)
                return r;

        if (u->meta.job)
                unit_serialize_item(u, f, "job", job_type_to_string(u->meta.job->type));

        dual_timestamp_serialize(f, "inactive-exit-timestamp", &u->meta.inactive_exit_timestamp);
        dual_timestamp_serialize(f, "active-enter-timestamp", &u->meta.active_enter_timestamp);
        dual_timestamp_serialize(f, "active-exit-timestamp", &u->meta.active_exit_timestamp);
        dual_timestamp_serialize(f, "inactive-enter-timestamp", &u->meta.inactive_enter_timestamp);
        dual_timestamp_serialize(f, "condition-timestamp", &u->meta.condition_timestamp);

        if (dual_timestamp_is_set(&u->meta.condition_timestamp))
                unit_serialize_item(u, f, "condition-result", yes_no(u->meta.condition_result));

        /* End marker */
        fputc('\n', f);
        return 0;
}

void unit_serialize_item_format(Unit *u, FILE *f, const char *key, const char *format, ...) {
        va_list ap;

        assert(u);
        assert(f);
        assert(key);
        assert(format);

        fputs(key, f);
        fputc('=', f);

        va_start(ap, format);
        vfprintf(f, format, ap);
        va_end(ap);

        fputc('\n', f);
}

void unit_serialize_item(Unit *u, FILE *f, const char *key, const char *value) {
        assert(u);
        assert(f);
        assert(key);
        assert(value);

        fprintf(f, "%s=%s\n", key, value);
}

int unit_deserialize(Unit *u, FILE *f, FDSet *fds) {
        int r;

        assert(u);
        assert(f);
        assert(fds);

        if (!unit_can_serialize(u))
                return 0;

        for (;;) {
                char line[LINE_MAX], *l, *v;
                size_t k;

                if (!fgets(line, sizeof(line), f)) {
                        if (feof(f))
                                return 0;
                        return -errno;
                }

                char_array_0(line);
                l = strstrip(line);

                /* End marker */
                if (l[0] == 0)
                        return 0;

                k = strcspn(l, "=");

                if (l[k] == '=') {
                        l[k] = 0;
                        v = l+k+1;
                } else
                        v = l+k;

                if (streq(l, "job")) {
                        JobType type;

                        if ((type = job_type_from_string(v)) < 0)
                                log_debug("Failed to parse job type value %s", v);
                        else
                                u->meta.deserialized_job = type;

                        continue;
                } else if (streq(l, "inactive-exit-timestamp")) {
                        dual_timestamp_deserialize(v, &u->meta.inactive_exit_timestamp);
                        continue;
                } else if (streq(l, "active-enter-timestamp")) {
                        dual_timestamp_deserialize(v, &u->meta.active_enter_timestamp);
                        continue;
                } else if (streq(l, "active-exit-timestamp")) {
                        dual_timestamp_deserialize(v, &u->meta.active_exit_timestamp);
                        continue;
                } else if (streq(l, "inactive-enter-timestamp")) {
                        dual_timestamp_deserialize(v, &u->meta.inactive_enter_timestamp);
                        continue;
                } else if (streq(l, "condition-timestamp")) {
                        dual_timestamp_deserialize(v, &u->meta.condition_timestamp);
                        continue;
                } else if (streq(l, "condition-result")) {
                        int b;

                        if ((b = parse_boolean(v)) < 0)
                                log_debug("Failed to parse condition result value %s", v);
                        else
                                u->meta.condition_result = b;

                        continue;
                }

                if ((r = UNIT_VTABLE(u)->deserialize_item(u, l, v, fds)) < 0)
                        return r;
        }
}

int unit_add_node_link(Unit *u, const char *what, bool wants) {
        Unit *device;
        char *e;
        int r;

        assert(u);

        if (!what)
                return 0;

        /* Adds in links to the device node that this unit is based on */

        if (!is_device_path(what))
                return 0;

        if (!(e = unit_name_build_escape(what+1, NULL, ".device")))
                return -ENOMEM;

        r = manager_load_unit(u->meta.manager, e, NULL, NULL, &device);
        free(e);

        if (r < 0)
                return r;

        if ((r = unit_add_two_dependencies(u, UNIT_AFTER, UNIT_BIND_TO, device, true)) < 0)
                return r;

        if (wants)
                if ((r = unit_add_dependency(device, UNIT_WANTS, u, false)) < 0)
                        return r;

        return 0;
}

int unit_coldplug(Unit *u) {
        int r;

        assert(u);

        if (UNIT_VTABLE(u)->coldplug)
                if ((r = UNIT_VTABLE(u)->coldplug(u)) < 0)
                        return r;

        if (u->meta.deserialized_job >= 0) {
                if ((r = manager_add_job(u->meta.manager, u->meta.deserialized_job, u, JOB_IGNORE_REQUIREMENTS, false, NULL, NULL)) < 0)
                        return r;

                u->meta.deserialized_job = _JOB_TYPE_INVALID;
        }

        return 0;
}

void unit_status_printf(Unit *u, const char *format, ...) {
        va_list ap;

        assert(u);
        assert(format);

        if (!UNIT_VTABLE(u)->show_status)
                return;

        if (u->meta.manager->running_as != MANAGER_SYSTEM)
                return;

        /* If Plymouth is running make sure we show the status, so
         * that there's something nice to see when people press Esc */

        if (!u->meta.manager->show_status && !plymouth_running())
                return;

        if (!manager_is_booting_or_shutting_down(u->meta.manager))
                return;

        va_start(ap, format);
        status_vprintf(format, ap);
        va_end(ap);
}

bool unit_need_daemon_reload(Unit *u) {
        struct stat st;

        assert(u);

        if (!u->meta.fragment_path)
                return false;

        zero(st);
        if (stat(u->meta.fragment_path, &st) < 0)
                /* What, cannot access this anymore? */
                return true;

        return
                u->meta.fragment_mtime &&
                timespec_load(&st.st_mtim) != u->meta.fragment_mtime;
}

void unit_reset_failed(Unit *u) {
        assert(u);

        if (UNIT_VTABLE(u)->reset_failed)
                UNIT_VTABLE(u)->reset_failed(u);
}

Unit *unit_following(Unit *u) {
        assert(u);

        if (UNIT_VTABLE(u)->following)
                return UNIT_VTABLE(u)->following(u);

        return NULL;
}

bool unit_pending_inactive(Unit *u) {
        assert(u);

        /* Returns true if the unit is inactive or going down */

        if (UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(u)))
                return true;

        if (u->meta.job && u->meta.job->type == JOB_STOP)
                return true;

        return false;
}

bool unit_pending_active(Unit *u) {
        assert(u);

        /* Returns true if the unit is inactive or going down */

        if (UNIT_IS_ACTIVE_OR_ACTIVATING(unit_active_state(u)))
                return true;

        if (u->meta.job &&
            (u->meta.job->type == JOB_START ||
             u->meta.job->type == JOB_RELOAD_OR_START ||
             u->meta.job->type == JOB_RESTART))
                return true;

        return false;
}

UnitType unit_name_to_type(const char *n) {
        UnitType t;

        assert(n);

        for (t = 0; t < _UNIT_TYPE_MAX; t++)
                if (endswith(n, unit_vtable[t]->suffix))
                        return t;

        return _UNIT_TYPE_INVALID;
}

bool unit_name_is_valid(const char *n, bool template_ok) {
        UnitType t;

        t = unit_name_to_type(n);
        if (t < 0 || t >= _UNIT_TYPE_MAX)
                return false;

        return unit_name_is_valid_no_type(n, template_ok);
}

int unit_kill(Unit *u, KillWho w, KillMode m, int signo, DBusError *error) {
        assert(u);
        assert(w >= 0 && w < _KILL_WHO_MAX);
        assert(m >= 0 && m < _KILL_MODE_MAX);
        assert(signo > 0);
        assert(signo < _NSIG);

        if (m == KILL_NONE)
                return 0;

        if (!UNIT_VTABLE(u)->kill)
                return -ENOTSUP;

        return UNIT_VTABLE(u)->kill(u, w, m, signo, error);
}


int unit_following_set(Unit *u, Set **s) {
        assert(u);
        assert(s);

        if (UNIT_VTABLE(u)->following_set)
                return UNIT_VTABLE(u)->following_set(u, s);

        *s = NULL;
        return 0;
}

static const char* const unit_load_state_table[_UNIT_LOAD_STATE_MAX] = {
        [UNIT_STUB] = "stub",
        [UNIT_LOADED] = "loaded",
        [UNIT_ERROR] = "error",
        [UNIT_MERGED] = "merged",
        [UNIT_MASKED] = "masked"
};

DEFINE_STRING_TABLE_LOOKUP(unit_load_state, UnitLoadState);

static const char* const unit_active_state_table[_UNIT_ACTIVE_STATE_MAX] = {
        [UNIT_ACTIVE] = "active",
        [UNIT_RELOADING] = "reloading",
        [UNIT_INACTIVE] = "inactive",
        [UNIT_FAILED] = "failed",
        [UNIT_ACTIVATING] = "activating",
        [UNIT_DEACTIVATING] = "deactivating"
};

DEFINE_STRING_TABLE_LOOKUP(unit_active_state, UnitActiveState);

static const char* const unit_dependency_table[_UNIT_DEPENDENCY_MAX] = {
        [UNIT_REQUIRES] = "Requires",
        [UNIT_REQUIRES_OVERRIDABLE] = "RequiresOverridable",
        [UNIT_WANTS] = "Wants",
        [UNIT_REQUISITE] = "Requisite",
        [UNIT_REQUISITE_OVERRIDABLE] = "RequisiteOverridable",
        [UNIT_REQUIRED_BY] = "RequiredBy",
        [UNIT_REQUIRED_BY_OVERRIDABLE] = "RequiredByOverridable",
        [UNIT_BIND_TO] = "BindTo",
        [UNIT_WANTED_BY] = "WantedBy",
        [UNIT_CONFLICTS] = "Conflicts",
        [UNIT_CONFLICTED_BY] = "ConflictedBy",
        [UNIT_BOUND_BY] = "BoundBy",
        [UNIT_BEFORE] = "Before",
        [UNIT_AFTER] = "After",
        [UNIT_REFERENCES] = "References",
        [UNIT_REFERENCED_BY] = "ReferencedBy",
        [UNIT_ON_FAILURE] = "OnFailure"
};

DEFINE_STRING_TABLE_LOOKUP(unit_dependency, UnitDependency);
