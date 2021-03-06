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

#include <errno.h>
#include <stdio.h>
#include <mntent.h>
#include <sys/epoll.h>
#include <signal.h>

#include "unit.h"
#include "mount.h"
#include "load-fragment.h"
#include "load-dropin.h"
#include "log.h"
#include "strv.h"
#include "mount-setup.h"
#include "unit-name.h"
#include "dbus-mount.h"
#include "special.h"
#include "bus-errors.h"
#include "exit-status.h"
#include "def.h"

static const UnitActiveState state_translation_table[_MOUNT_STATE_MAX] = {
        [MOUNT_DEAD] = UNIT_INACTIVE,
        [MOUNT_MOUNTING] = UNIT_ACTIVATING,
        [MOUNT_MOUNTING_DONE] = UNIT_ACTIVE,
        [MOUNT_MOUNTED] = UNIT_ACTIVE,
        [MOUNT_REMOUNTING] = UNIT_RELOADING,
        [MOUNT_UNMOUNTING] = UNIT_DEACTIVATING,
        [MOUNT_MOUNTING_SIGTERM] = UNIT_DEACTIVATING,
        [MOUNT_MOUNTING_SIGKILL] = UNIT_DEACTIVATING,
        [MOUNT_REMOUNTING_SIGTERM] = UNIT_RELOADING,
        [MOUNT_REMOUNTING_SIGKILL] = UNIT_RELOADING,
        [MOUNT_UNMOUNTING_SIGTERM] = UNIT_DEACTIVATING,
        [MOUNT_UNMOUNTING_SIGKILL] = UNIT_DEACTIVATING,
        [MOUNT_FAILED] = UNIT_FAILED
};

static void mount_init(Unit *u) {
        Mount *m = MOUNT(u);

        assert(u);
        assert(u->meta.load_state == UNIT_STUB);

        m->timeout_usec = DEFAULT_TIMEOUT_USEC;
        m->directory_mode = 0755;

        exec_context_init(&m->exec_context);

        /* The stdio/kmsg bridge socket is on /, in order to avoid a
         * dep loop, don't use kmsg logging for -.mount */
        if (!unit_has_name(u, "-.mount"))
                m->exec_context.std_output = EXEC_OUTPUT_KMSG;

        /* We need to make sure that /bin/mount is always called in
         * the same process group as us, so that the autofs kernel
         * side doesn't send us another mount request while we are
         * already trying to comply its last one. */
        m->exec_context.same_pgrp = true;

        m->timer_watch.type = WATCH_INVALID;

        m->control_command_id = _MOUNT_EXEC_COMMAND_INVALID;

        m->meta.ignore_on_isolate = true;
}

static void mount_unwatch_control_pid(Mount *m) {
        assert(m);

        if (m->control_pid <= 0)
                return;

        unit_unwatch_pid(UNIT(m), m->control_pid);
        m->control_pid = 0;
}

static void mount_parameters_done(MountParameters *p) {
        assert(p);

        free(p->what);
        free(p->options);
        free(p->fstype);

        p->what = p->options = p->fstype = NULL;
}

static void mount_done(Unit *u) {
        Mount *m = MOUNT(u);
        Meta *other;

        assert(m);

        free(m->where);
        m->where = NULL;

        /* Try to detach us from the automount unit if there is any */
        LIST_FOREACH(units_per_type, other, m->meta.manager->units_per_type[UNIT_AUTOMOUNT]) {
                Automount *a = (Automount*) other;

                if (a->mount == m)
                        a->mount = NULL;
        }

        mount_parameters_done(&m->parameters_etc_fstab);
        mount_parameters_done(&m->parameters_proc_self_mountinfo);
        mount_parameters_done(&m->parameters_fragment);

        exec_context_done(&m->exec_context);
        exec_command_done_array(m->exec_command, _MOUNT_EXEC_COMMAND_MAX);
        m->control_command = NULL;

        mount_unwatch_control_pid(m);

        unit_unwatch_timer(u, &m->timer_watch);
}

static MountParameters* get_mount_parameters_configured(Mount *m) {
        assert(m);

        if (m->from_fragment)
                return &m->parameters_fragment;
        else if (m->from_etc_fstab)
                return &m->parameters_etc_fstab;

        return NULL;
}

static MountParameters* get_mount_parameters(Mount *m) {
        assert(m);

        if (m->from_proc_self_mountinfo)
                return &m->parameters_proc_self_mountinfo;

        return get_mount_parameters_configured(m);
}

static int mount_add_mount_links(Mount *m) {
        Meta *other;
        int r;
        MountParameters *pm;

        assert(m);

        pm = get_mount_parameters_configured(m);

        /* Adds in links to other mount points that might lie below or
         * above us in the hierarchy */

        LIST_FOREACH(units_per_type, other, m->meta.manager->units_per_type[UNIT_MOUNT]) {
                Mount *n = (Mount*) other;
                MountParameters *pn;

                if (n == m)
                        continue;

                if (n->meta.load_state != UNIT_LOADED)
                        continue;

                pn = get_mount_parameters_configured(n);

                if (path_startswith(m->where, n->where)) {

                        if ((r = unit_add_dependency(UNIT(m), UNIT_AFTER, UNIT(n), true)) < 0)
                                return r;

                        if (pn)
                                if ((r = unit_add_dependency(UNIT(m), UNIT_REQUIRES, UNIT(n), true)) < 0)
                                        return r;

                } else if (path_startswith(n->where, m->where)) {

                        if ((r = unit_add_dependency(UNIT(n), UNIT_AFTER, UNIT(m), true)) < 0)
                                return r;

                        if (pm)
                                if ((r = unit_add_dependency(UNIT(n), UNIT_REQUIRES, UNIT(m), true)) < 0)
                                        return r;

                } else if (pm && path_startswith(pm->what, n->where)) {

                        if ((r = unit_add_dependency(UNIT(m), UNIT_AFTER, UNIT(n), true)) < 0)
                                return r;

                        if ((r = unit_add_dependency(UNIT(m), UNIT_REQUIRES, UNIT(n), true)) < 0)
                                return r;

                } else if (pn && path_startswith(pn->what, m->where)) {

                        if ((r = unit_add_dependency(UNIT(n), UNIT_AFTER, UNIT(m), true)) < 0)
                                return r;

                        if ((r = unit_add_dependency(UNIT(n), UNIT_REQUIRES, UNIT(m), true)) < 0)
                                return r;
                }
        }

        return 0;
}

static int mount_add_swap_links(Mount *m) {
        Meta *other;
        int r;

        assert(m);

        LIST_FOREACH(units_per_type, other, m->meta.manager->units_per_type[UNIT_SWAP])
                if ((r = swap_add_one_mount_link((Swap*) other, m)) < 0)
                        return r;

        return 0;
}

static int mount_add_path_links(Mount *m) {
        Meta *other;
        int r;

        assert(m);

        LIST_FOREACH(units_per_type, other, m->meta.manager->units_per_type[UNIT_PATH])
                if ((r = path_add_one_mount_link((Path*) other, m)) < 0)
                        return r;

        return 0;
}

static int mount_add_automount_links(Mount *m) {
        Meta *other;
        int r;

        assert(m);

        LIST_FOREACH(units_per_type, other, m->meta.manager->units_per_type[UNIT_AUTOMOUNT])
                if ((r = automount_add_one_mount_link((Automount*) other, m)) < 0)
                        return r;

        return 0;
}

static int mount_add_socket_links(Mount *m) {
        Meta *other;
        int r;

        assert(m);

        LIST_FOREACH(units_per_type, other, m->meta.manager->units_per_type[UNIT_SOCKET])
                if ((r = socket_add_one_mount_link((Socket*) other, m)) < 0)
                        return r;

        return 0;
}

static char* mount_test_option(const char *haystack, const char *needle) {
        struct mntent me;

        assert(needle);

        /* Like glibc's hasmntopt(), but works on a string, not a
         * struct mntent */

        if (!haystack)
                return false;

        zero(me);
        me.mnt_opts = (char*) haystack;

        return hasmntopt(&me, needle);
}

static bool mount_is_network(MountParameters *p) {
        assert(p);

        if (mount_test_option(p->options, "_netdev"))
                return true;

        if (p->fstype && fstype_is_network(p->fstype))
                return true;

        return false;
}

static bool mount_is_bind(MountParameters *p) {
        assert(p);

        if (mount_test_option(p->options, "bind"))
                return true;

        if (p->fstype && streq(p->fstype, "bind"))
                return true;

        return false;
}

static bool needs_quota(MountParameters *p) {
        assert(p);

        if (mount_is_network(p))
                return false;

        if (mount_is_bind(p))
                return false;

        return mount_test_option(p->options, "usrquota") ||
                mount_test_option(p->options, "grpquota");
}

static int mount_add_fstab_links(Mount *m) {
        const char *target, *after = NULL;
        MountParameters *p;
        Unit *tu;
        int r;
        bool noauto, nofail, handle, automount;

        assert(m);

        if (m->meta.manager->running_as != MANAGER_SYSTEM)
                return 0;

        if (!(p = get_mount_parameters_configured(m)))
                return 0;

        if (p != &m->parameters_etc_fstab)
                return 0;

        noauto = !!mount_test_option(p->options, "noauto");
        nofail = !!mount_test_option(p->options, "nofail");
        automount =
                mount_test_option(p->options, "comment=systemd.automount") ||
                mount_test_option(p->options, "x-systemd-automount");
        handle =
                automount ||
                mount_test_option(p->options, "comment=systemd.mount") ||
                mount_test_option(p->options, "x-systemd-mount") ||
                m->meta.manager->mount_auto;

        if (mount_is_network(p)) {
                target = SPECIAL_REMOTE_FS_TARGET;
                after = SPECIAL_NETWORK_TARGET;
        } else
                target = SPECIAL_LOCAL_FS_TARGET;

        if (!path_equal(m->where, "/"))
                if ((r = unit_add_two_dependencies_by_name(UNIT(m), UNIT_BEFORE, UNIT_CONFLICTS, SPECIAL_UMOUNT_TARGET, NULL, true)) < 0)
                        return r;

        if ((r = manager_load_unit(m->meta.manager, target, NULL, NULL, &tu)) < 0)
                return r;

        if (after)
                if ((r = unit_add_dependency_by_name(UNIT(m), UNIT_AFTER, after, NULL, true)) < 0)
                        return r;

        if (automount) {
                Unit *am;

                if ((r = unit_load_related_unit(UNIT(m), ".automount", &am)) < 0)
                        return r;

                /* If auto is configured as well also pull in the
                 * mount right-away, but don't rely on it. */
                if (!noauto) /* automount + auto */
                        if ((r = unit_add_dependency(tu, UNIT_WANTS, UNIT(m), true)) < 0)
                                return r;

                /* Install automount unit */
                if (!nofail) /* automount + fail */
                        return unit_add_two_dependencies(tu, UNIT_AFTER, UNIT_REQUIRES, UNIT(am), true);
                else /* automount + nofail */
                        return unit_add_two_dependencies(tu, UNIT_AFTER, UNIT_WANTS, UNIT(am), true);

        } else if (handle && !noauto) {

                /* Automatically add mount points that aren't natively
                 * configured to local-fs.target */

                if (!nofail) /* auto + fail */
                        return unit_add_two_dependencies(tu, UNIT_AFTER, UNIT_REQUIRES, UNIT(m), true);
                else /* auto + nofail */
                        return unit_add_dependency(tu, UNIT_WANTS, UNIT(m), true);
        }

        return 0;
}

static int mount_add_device_links(Mount *m) {
        MountParameters *p;
        int r;

        assert(m);

        if (!(p = get_mount_parameters_configured(m)))
                return 0;

        if (!p->what)
                return 0;

        if (!mount_is_bind(p) &&
            !path_equal(m->where, "/") &&
            p == &m->parameters_etc_fstab) {
                bool nofail, noauto;

                noauto = !!mount_test_option(p->options, "noauto");
                nofail = !!mount_test_option(p->options, "nofail");

                if ((r = unit_add_node_link(UNIT(m), p->what,
                                            !noauto && nofail &&
                                            UNIT(m)->meta.manager->running_as == MANAGER_SYSTEM)) < 0)
                        return r;
        }

        if (p->passno > 0 &&
            !mount_is_bind(p) &&
            UNIT(m)->meta.manager->running_as == MANAGER_SYSTEM &&
            !path_equal(m->where, "/")) {
                char *name;
                Unit *fsck;
                /* Let's add in the fsck service */

                /* aka SPECIAL_FSCK_SERVICE */
                if (!(name = unit_name_from_path_instance("fsck", p->what, ".service")))
                        return -ENOMEM;

                if ((r = manager_load_unit_prepare(m->meta.manager, name, NULL, NULL, &fsck)) < 0) {
                        log_warning("Failed to prepare unit %s: %s", name, strerror(-r));
                        free(name);
                        return r;
                }

                free(name);

                SERVICE(fsck)->fsck_passno = p->passno;

                if ((r = unit_add_two_dependencies(UNIT(m), UNIT_AFTER, UNIT_REQUIRES, fsck, true)) < 0)
                        return r;
        }

        return 0;
}

static int mount_add_default_dependencies(Mount *m) {
        int r;

        assert(m);

        if (m->meta.manager->running_as == MANAGER_SYSTEM &&
            !path_equal(m->where, "/")) {
                MountParameters *p;

                p = get_mount_parameters_configured(m);

                if (p && needs_quota(p)) {
                        if ((r = unit_add_two_dependencies_by_name(UNIT(m), UNIT_BEFORE, UNIT_WANTS, SPECIAL_QUOTACHECK_SERVICE, NULL, true)) < 0 ||
                            (r = unit_add_two_dependencies_by_name(UNIT(m), UNIT_BEFORE, UNIT_WANTS, SPECIAL_QUOTAON_SERVICE, NULL, true)) < 0)
                                return r;
                }

                if ((r = unit_add_two_dependencies_by_name(UNIT(m), UNIT_BEFORE, UNIT_CONFLICTS, SPECIAL_UMOUNT_TARGET, NULL, true)) < 0)
                        return r;
        }

        return 0;
}

static int mount_fix_timeouts(Mount *m) {
        MountParameters *p;
        const char *timeout = NULL;
        Unit *other;
        Iterator i;
        usec_t u;
        char *t;
        int r;

        assert(m);

        if (!(p = get_mount_parameters_configured(m)))
                return 0;

        /* Allow configuration how long we wait for a device that
         * backs a mount point to show up. This is useful to support
         * endless device timeouts for devices that show up only after
         * user input, like crypto devices. */

        if ((timeout = mount_test_option(p->options, "comment=systemd.device-timeout")))
                timeout += 31;
        else if ((timeout = mount_test_option(p->options, "x-systemd-device-timeout")))
                timeout += 25;
        else
                return 0;

        t = strndup(timeout, strcspn(timeout, ",;" WHITESPACE));
        if (!t)
                return -ENOMEM;

        r = parse_usec(t, &u);
        free(t);

        if (r < 0) {
                log_warning("Failed to parse timeout for %s, ignoring: %s", m->where, timeout);
                return r;
        }

        SET_FOREACH(other, m->meta.dependencies[UNIT_AFTER], i) {
                if (other->meta.type != UNIT_DEVICE)
                        continue;

                other->meta.job_timeout = u;
        }

        return 0;
}

static int mount_verify(Mount *m) {
        bool b;
        char *e;
        assert(m);

        if (m->meta.load_state != UNIT_LOADED)
                return 0;

        if (!m->from_etc_fstab && !m->from_fragment && !m->from_proc_self_mountinfo)
                return -ENOENT;

        if (!(e = unit_name_from_path(m->where, ".mount")))
                return -ENOMEM;

        b = unit_has_name(UNIT(m), e);
        free(e);

        if (!b) {
                log_error("%s's Where setting doesn't match unit name. Refusing.", m->meta.id);
                return -EINVAL;
        }

        if (mount_point_is_api(m->where) || mount_point_ignore(m->where)) {
                log_error("Cannot create mount unit for API file system %s. Refusing.", m->where);
                return -EINVAL;
        }

        if (m->meta.fragment_path && !m->parameters_fragment.what) {
                log_error("%s's What setting is missing. Refusing.", m->meta.id);
                return -EBADMSG;
        }

        if (m->exec_context.pam_name && m->exec_context.kill_mode != KILL_CONTROL_GROUP) {
                log_error("%s has PAM enabled. Kill mode must be set to 'control-group'. Refusing.", m->meta.id);
                return -EINVAL;
        }

        return 0;
}

static int mount_load(Unit *u) {
        Mount *m = MOUNT(u);
        int r;

        assert(u);
        assert(u->meta.load_state == UNIT_STUB);

        if ((r = unit_load_fragment_and_dropin_optional(u)) < 0)
                return r;

        /* This is a new unit? Then let's add in some extras */
        if (u->meta.load_state == UNIT_LOADED) {
                if ((r = unit_add_exec_dependencies(u, &m->exec_context)) < 0)
                        return r;

                if (m->meta.fragment_path)
                        m->from_fragment = true;
                else if (m->from_etc_fstab)
                        m->meta.default_dependencies = false;

                if (!m->where)
                        if (!(m->where = unit_name_to_path(u->meta.id)))
                                return -ENOMEM;

                path_kill_slashes(m->where);

                if (!m->meta.description)
                        if ((r = unit_set_description(u, m->where)) < 0)
                                return r;

                if ((r = mount_add_device_links(m)) < 0)
                        return r;

                if ((r = mount_add_mount_links(m)) < 0)
                        return r;

                if ((r = mount_add_socket_links(m)) < 0)
                        return r;

                if ((r = mount_add_swap_links(m)) < 0)
                        return r;

                if ((r = mount_add_path_links(m)) < 0)
                        return r;

                if ((r = mount_add_automount_links(m)) < 0)
                        return r;

                if ((r = mount_add_fstab_links(m)) < 0)
                        return r;

                if (m->meta.default_dependencies)
                        if ((r = mount_add_default_dependencies(m)) < 0)
                                return r;

                if ((r = unit_add_default_cgroups(u)) < 0)
                        return r;

                mount_fix_timeouts(m);
        }

        return mount_verify(m);
}

static int mount_notify_automount(Mount *m, int status) {
        Unit *p;
        int r;

        assert(m);

        if ((r = unit_get_related_unit(UNIT(m), ".automount", &p)) < 0)
                return r == -ENOENT ? 0 : r;

        return automount_send_ready(AUTOMOUNT(p), status);
}

static void mount_set_state(Mount *m, MountState state) {
        MountState old_state;
        assert(m);

        old_state = m->state;
        m->state = state;

        if (state != MOUNT_MOUNTING &&
            state != MOUNT_MOUNTING_DONE &&
            state != MOUNT_REMOUNTING &&
            state != MOUNT_UNMOUNTING &&
            state != MOUNT_MOUNTING_SIGTERM &&
            state != MOUNT_MOUNTING_SIGKILL &&
            state != MOUNT_UNMOUNTING_SIGTERM &&
            state != MOUNT_UNMOUNTING_SIGKILL &&
            state != MOUNT_REMOUNTING_SIGTERM &&
            state != MOUNT_REMOUNTING_SIGKILL) {
                unit_unwatch_timer(UNIT(m), &m->timer_watch);
                mount_unwatch_control_pid(m);
                m->control_command = NULL;
                m->control_command_id = _MOUNT_EXEC_COMMAND_INVALID;
        }

        if (state == MOUNT_MOUNTED ||
            state == MOUNT_REMOUNTING)
                mount_notify_automount(m, 0);
        else if (state == MOUNT_DEAD ||
                 state == MOUNT_UNMOUNTING ||
                 state == MOUNT_MOUNTING_SIGTERM ||
                 state == MOUNT_MOUNTING_SIGKILL ||
                 state == MOUNT_REMOUNTING_SIGTERM ||
                 state == MOUNT_REMOUNTING_SIGKILL ||
                 state == MOUNT_UNMOUNTING_SIGTERM ||
                 state == MOUNT_UNMOUNTING_SIGKILL ||
                 state == MOUNT_FAILED)
                mount_notify_automount(m, -ENODEV);

        if (state != old_state)
                log_debug("%s changed %s -> %s",
                          m->meta.id,
                          mount_state_to_string(old_state),
                          mount_state_to_string(state));

        unit_notify(UNIT(m), state_translation_table[old_state], state_translation_table[state], !m->reload_failure);
        m->reload_failure = false;
}

static int mount_coldplug(Unit *u) {
        Mount *m = MOUNT(u);
        MountState new_state = MOUNT_DEAD;
        int r;

        assert(m);
        assert(m->state == MOUNT_DEAD);

        if (m->deserialized_state != m->state)
                new_state = m->deserialized_state;
        else if (m->from_proc_self_mountinfo)
                new_state = MOUNT_MOUNTED;

        if (new_state != m->state) {

                if (new_state == MOUNT_MOUNTING ||
                    new_state == MOUNT_MOUNTING_DONE ||
                    new_state == MOUNT_REMOUNTING ||
                    new_state == MOUNT_UNMOUNTING ||
                    new_state == MOUNT_MOUNTING_SIGTERM ||
                    new_state == MOUNT_MOUNTING_SIGKILL ||
                    new_state == MOUNT_UNMOUNTING_SIGTERM ||
                    new_state == MOUNT_UNMOUNTING_SIGKILL ||
                    new_state == MOUNT_REMOUNTING_SIGTERM ||
                    new_state == MOUNT_REMOUNTING_SIGKILL) {

                        if (m->control_pid <= 0)
                                return -EBADMSG;

                        if ((r = unit_watch_pid(UNIT(m), m->control_pid)) < 0)
                                return r;

                        if ((r = unit_watch_timer(UNIT(m), m->timeout_usec, &m->timer_watch)) < 0)
                                return r;
                }

                mount_set_state(m, new_state);
        }

        return 0;
}

static void mount_dump(Unit *u, FILE *f, const char *prefix) {
        Mount *m = MOUNT(u);
        MountParameters *p;

        assert(m);
        assert(f);

        p = get_mount_parameters(m);

        fprintf(f,
                "%sMount State: %s\n"
                "%sWhere: %s\n"
                "%sWhat: %s\n"
                "%sFile System Type: %s\n"
                "%sOptions: %s\n"
                "%sFrom /etc/fstab: %s\n"
                "%sFrom /proc/self/mountinfo: %s\n"
                "%sFrom fragment: %s\n"
                "%sDirectoryMode: %04o\n",
                prefix, mount_state_to_string(m->state),
                prefix, m->where,
                prefix, strna(p->what),
                prefix, strna(p->fstype),
                prefix, strna(p->options),
                prefix, yes_no(m->from_etc_fstab),
                prefix, yes_no(m->from_proc_self_mountinfo),
                prefix, yes_no(m->from_fragment),
                prefix, m->directory_mode);

        if (m->control_pid > 0)
                fprintf(f,
                        "%sControl PID: %lu\n",
                        prefix, (unsigned long) m->control_pid);

        exec_context_dump(&m->exec_context, f, prefix);
}

static int mount_spawn(Mount *m, ExecCommand *c, pid_t *_pid) {
        pid_t pid;
        int r;

        assert(m);
        assert(c);
        assert(_pid);

        if ((r = unit_watch_timer(UNIT(m), m->timeout_usec, &m->timer_watch)) < 0)
                goto fail;

        if ((r = exec_spawn(c,
                            NULL,
                            &m->exec_context,
                            NULL, 0,
                            m->meta.manager->environment,
                            true,
                            true,
                            true,
                            m->meta.manager->confirm_spawn,
                            m->meta.cgroup_bondings,
                            &pid)) < 0)
                goto fail;

        if ((r = unit_watch_pid(UNIT(m), pid)) < 0)
                /* FIXME: we need to do something here */
                goto fail;

        *_pid = pid;

        return 0;

fail:
        unit_unwatch_timer(UNIT(m), &m->timer_watch);

        return r;
}

static void mount_enter_dead(Mount *m, bool success) {
        assert(m);

        if (!success)
                m->failure = true;

        mount_set_state(m, m->failure ? MOUNT_FAILED : MOUNT_DEAD);
}

static void mount_enter_mounted(Mount *m, bool success) {
        assert(m);

        if (!success)
                m->failure = true;

        mount_set_state(m, MOUNT_MOUNTED);
}

static void mount_enter_signal(Mount *m, MountState state, bool success) {
        int r;
        Set *pid_set = NULL;
        bool wait_for_exit = false;

        assert(m);

        if (!success)
                m->failure = true;

        if (m->exec_context.kill_mode != KILL_NONE) {
                int sig = (state == MOUNT_MOUNTING_SIGTERM ||
                           state == MOUNT_UNMOUNTING_SIGTERM ||
                           state == MOUNT_REMOUNTING_SIGTERM) ? m->exec_context.kill_signal : SIGKILL;

                if (m->control_pid > 0) {
                        if (kill_and_sigcont(m->control_pid, sig) < 0 && errno != ESRCH)

                                log_warning("Failed to kill control process %li: %m", (long) m->control_pid);
                        else
                                wait_for_exit = true;
                }

                if (m->exec_context.kill_mode == KILL_CONTROL_GROUP) {

                        if (!(pid_set = set_new(trivial_hash_func, trivial_compare_func))) {
                                r = -ENOMEM;
                                goto fail;
                        }

                        /* Exclude the control pid from being killed via the cgroup */
                        if (m->control_pid > 0)
                                if ((r = set_put(pid_set, LONG_TO_PTR(m->control_pid))) < 0)
                                        goto fail;

                        if ((r = cgroup_bonding_kill_list(m->meta.cgroup_bondings, sig, true, pid_set)) < 0) {
                                if (r != -EAGAIN && r != -ESRCH && r != -ENOENT)
                                        log_warning("Failed to kill control group: %s", strerror(-r));
                        } else if (r > 0)
                                wait_for_exit = true;

                        set_free(pid_set);
                        pid_set = NULL;
                }
        }

        if (wait_for_exit) {
                if ((r = unit_watch_timer(UNIT(m), m->timeout_usec, &m->timer_watch)) < 0)
                        goto fail;

                mount_set_state(m, state);
        } else if (state == MOUNT_REMOUNTING_SIGTERM || state == MOUNT_REMOUNTING_SIGKILL)
                mount_enter_mounted(m, true);
        else
                mount_enter_dead(m, true);

        return;

fail:
        log_warning("%s failed to kill processes: %s", m->meta.id, strerror(-r));

        if (state == MOUNT_REMOUNTING_SIGTERM || state == MOUNT_REMOUNTING_SIGKILL)
                mount_enter_mounted(m, false);
        else
                mount_enter_dead(m, false);

        if (pid_set)
                set_free(pid_set);
}

static void mount_enter_unmounting(Mount *m, bool success) {
        int r;

        assert(m);

        if (!success)
                m->failure = true;

        m->control_command_id = MOUNT_EXEC_UNMOUNT;
        m->control_command = m->exec_command + MOUNT_EXEC_UNMOUNT;

        if ((r = exec_command_set(
                             m->control_command,
                             "/bin/umount",
                             m->where,
                             NULL)) < 0)
                goto fail;

        mount_unwatch_control_pid(m);

        if ((r = mount_spawn(m, m->control_command, &m->control_pid)) < 0)
                goto fail;

        mount_set_state(m, MOUNT_UNMOUNTING);

        return;

fail:
        log_warning("%s failed to run 'umount' task: %s", m->meta.id, strerror(-r));
        mount_enter_mounted(m, false);
}

static void mount_enter_mounting(Mount *m) {
        int r;
        MountParameters *p;

        assert(m);

        m->control_command_id = MOUNT_EXEC_MOUNT;
        m->control_command = m->exec_command + MOUNT_EXEC_MOUNT;

        mkdir_p(m->where, m->directory_mode);

        /* Create the source directory for bind-mounts if needed */
        p = get_mount_parameters_configured(m);
        if (p && mount_is_bind(p))
                mkdir_p(p->what, m->directory_mode);

        if (m->from_fragment)
                r = exec_command_set(
                                m->control_command,
                                "/bin/mount",
                                m->parameters_fragment.what,
                                m->where,
                                "-t", m->parameters_fragment.fstype ? m->parameters_fragment.fstype : "auto",
                                m->parameters_fragment.options ? "-o" : NULL, m->parameters_fragment.options,
                                NULL);
        else if (m->from_etc_fstab)
                r = exec_command_set(
                                m->control_command,
                                "/bin/mount",
                                m->where,
                                NULL);
        else
                r = -ENOENT;

        if (r < 0)
                goto fail;

        mount_unwatch_control_pid(m);

        if ((r = mount_spawn(m, m->control_command, &m->control_pid)) < 0)
                goto fail;

        mount_set_state(m, MOUNT_MOUNTING);

        return;

fail:
        log_warning("%s failed to run 'mount' task: %s", m->meta.id, strerror(-r));
        mount_enter_dead(m, false);
}

static void mount_enter_mounting_done(Mount *m) {
        assert(m);

        mount_set_state(m, MOUNT_MOUNTING_DONE);
}

static void mount_enter_remounting(Mount *m, bool success) {
        int r;

        assert(m);

        if (!success)
                m->failure = true;

        m->control_command_id = MOUNT_EXEC_REMOUNT;
        m->control_command = m->exec_command + MOUNT_EXEC_REMOUNT;

        if (m->from_fragment) {
                char *buf = NULL;
                const char *o;

                if (m->parameters_fragment.options) {
                        if (!(buf = strappend("remount,", m->parameters_fragment.options))) {
                                r = -ENOMEM;
                                goto fail;
                        }

                        o = buf;
                } else
                        o = "remount";

                r = exec_command_set(
                                m->control_command,
                                "/bin/mount",
                                m->parameters_fragment.what,
                                m->where,
                                "-t", m->parameters_fragment.fstype ? m->parameters_fragment.fstype : "auto",
                                "-o", o,
                                NULL);

                free(buf);
        } else if (m->from_etc_fstab)
                r = exec_command_set(
                                m->control_command,
                                "/bin/mount",
                                m->where,
                                "-o", "remount",
                                NULL);
        else
                r = -ENOENT;

        if (r < 0)
                goto fail;

        mount_unwatch_control_pid(m);

        if ((r = mount_spawn(m, m->control_command, &m->control_pid)) < 0)
                goto fail;

        mount_set_state(m, MOUNT_REMOUNTING);

        return;

fail:
        log_warning("%s failed to run 'remount' task: %s", m->meta.id, strerror(-r));
        m->reload_failure = true;
        mount_enter_mounted(m, true);
}

static int mount_start(Unit *u) {
        Mount *m = MOUNT(u);

        assert(m);

        /* We cannot fulfill this request right now, try again later
         * please! */
        if (m->state == MOUNT_UNMOUNTING ||
            m->state == MOUNT_UNMOUNTING_SIGTERM ||
            m->state == MOUNT_UNMOUNTING_SIGKILL ||
            m->state == MOUNT_MOUNTING_SIGTERM ||
            m->state == MOUNT_MOUNTING_SIGKILL)
                return -EAGAIN;

        /* Already on it! */
        if (m->state == MOUNT_MOUNTING)
                return 0;

        assert(m->state == MOUNT_DEAD || m->state == MOUNT_FAILED);

        m->failure = false;
        mount_enter_mounting(m);
        return 0;
}

static int mount_stop(Unit *u) {
        Mount *m = MOUNT(u);

        assert(m);

        /* Already on it */
        if (m->state == MOUNT_UNMOUNTING ||
            m->state == MOUNT_UNMOUNTING_SIGKILL ||
            m->state == MOUNT_UNMOUNTING_SIGTERM ||
            m->state == MOUNT_MOUNTING_SIGTERM ||
            m->state == MOUNT_MOUNTING_SIGKILL)
                return 0;

        assert(m->state == MOUNT_MOUNTING ||
               m->state == MOUNT_MOUNTING_DONE ||
               m->state == MOUNT_MOUNTED ||
               m->state == MOUNT_REMOUNTING ||
               m->state == MOUNT_REMOUNTING_SIGTERM ||
               m->state == MOUNT_REMOUNTING_SIGKILL);

        mount_enter_unmounting(m, true);
        return 0;
}

static int mount_reload(Unit *u) {
        Mount *m = MOUNT(u);

        assert(m);

        if (m->state == MOUNT_MOUNTING_DONE)
                return -EAGAIN;

        assert(m->state == MOUNT_MOUNTED);

        mount_enter_remounting(m, true);
        return 0;
}

static int mount_serialize(Unit *u, FILE *f, FDSet *fds) {
        Mount *m = MOUNT(u);

        assert(m);
        assert(f);
        assert(fds);

        unit_serialize_item(u, f, "state", mount_state_to_string(m->state));
        unit_serialize_item(u, f, "failure", yes_no(m->failure));

        if (m->control_pid > 0)
                unit_serialize_item_format(u, f, "control-pid", "%lu", (unsigned long) m->control_pid);

        if (m->control_command_id >= 0)
                unit_serialize_item(u, f, "control-command", mount_exec_command_to_string(m->control_command_id));

        return 0;
}

static int mount_deserialize_item(Unit *u, const char *key, const char *value, FDSet *fds) {
        Mount *m = MOUNT(u);

        assert(u);
        assert(key);
        assert(value);
        assert(fds);

        if (streq(key, "state")) {
                MountState state;

                if ((state = mount_state_from_string(value)) < 0)
                        log_debug("Failed to parse state value %s", value);
                else
                        m->deserialized_state = state;
        } else if (streq(key, "failure")) {
                int b;

                if ((b = parse_boolean(value)) < 0)
                        log_debug("Failed to parse failure value %s", value);
                else
                        m->failure = b || m->failure;

        } else if (streq(key, "control-pid")) {
                pid_t pid;

                if (parse_pid(value, &pid) < 0)
                        log_debug("Failed to parse control-pid value %s", value);
                else
                        m->control_pid = pid;
        } else if (streq(key, "control-command")) {
                MountExecCommand id;

                if ((id = mount_exec_command_from_string(value)) < 0)
                        log_debug("Failed to parse exec-command value %s", value);
                else {
                        m->control_command_id = id;
                        m->control_command = m->exec_command + id;
                }

        } else
                log_debug("Unknown serialization key '%s'", key);

        return 0;
}

static UnitActiveState mount_active_state(Unit *u) {
        assert(u);

        return state_translation_table[MOUNT(u)->state];
}

static const char *mount_sub_state_to_string(Unit *u) {
        assert(u);

        return mount_state_to_string(MOUNT(u)->state);
}

static bool mount_check_gc(Unit *u) {
        Mount *m = MOUNT(u);

        assert(m);

        return m->from_etc_fstab || m->from_proc_self_mountinfo;
}

static void mount_sigchld_event(Unit *u, pid_t pid, int code, int status) {
        Mount *m = MOUNT(u);
        bool success;

        assert(m);
        assert(pid >= 0);

        if (pid != m->control_pid)
                return;

        m->control_pid = 0;

        success = is_clean_exit(code, status);
        m->failure = m->failure || !success;

        if (m->control_command) {
                exec_status_exit(&m->control_command->exec_status, &m->exec_context, pid, code, status);
                m->control_command = NULL;
                m->control_command_id = _MOUNT_EXEC_COMMAND_INVALID;
        }

        log_full(success ? LOG_DEBUG : LOG_NOTICE,
                 "%s mount process exited, code=%s status=%i", u->meta.id, sigchld_code_to_string(code), status);

        /* Note that mount(8) returning and the kernel sending us a
         * mount table change event might happen out-of-order. If an
         * operation succeed we assume the kernel will follow soon too
         * and already change into the resulting state.  If it fails
         * we check if the kernel still knows about the mount. and
         * change state accordingly. */

        switch (m->state) {

        case MOUNT_MOUNTING:
        case MOUNT_MOUNTING_DONE:
        case MOUNT_MOUNTING_SIGKILL:
        case MOUNT_MOUNTING_SIGTERM:

                if (success)
                        mount_enter_mounted(m, true);
                else if (m->from_proc_self_mountinfo)
                        mount_enter_mounted(m, false);
                else
                        mount_enter_dead(m, false);
                break;

        case MOUNT_REMOUNTING:
        case MOUNT_REMOUNTING_SIGKILL:
        case MOUNT_REMOUNTING_SIGTERM:

                m->reload_failure = !success;
                if (m->from_proc_self_mountinfo)
                        mount_enter_mounted(m, true);
                else
                        mount_enter_dead(m, true);

                break;

        case MOUNT_UNMOUNTING:
        case MOUNT_UNMOUNTING_SIGKILL:
        case MOUNT_UNMOUNTING_SIGTERM:

                if (success)
                        mount_enter_dead(m, true);
                else if (m->from_proc_self_mountinfo)
                        mount_enter_mounted(m, false);
                else
                        mount_enter_dead(m, false);
                break;

        default:
                assert_not_reached("Uh, control process died at wrong time.");
        }

        /* Notify clients about changed exit status */
        unit_add_to_dbus_queue(u);
}

static void mount_timer_event(Unit *u, uint64_t elapsed, Watch *w) {
        Mount *m = MOUNT(u);

        assert(m);
        assert(elapsed == 1);
        assert(w == &m->timer_watch);

        switch (m->state) {

        case MOUNT_MOUNTING:
        case MOUNT_MOUNTING_DONE:
                log_warning("%s mounting timed out. Stopping.", u->meta.id);
                mount_enter_signal(m, MOUNT_MOUNTING_SIGTERM, false);
                break;

        case MOUNT_REMOUNTING:
                log_warning("%s remounting timed out. Stopping.", u->meta.id);
                m->reload_failure = true;
                mount_enter_mounted(m, true);
                break;

        case MOUNT_UNMOUNTING:
                log_warning("%s unmounting timed out. Stopping.", u->meta.id);
                mount_enter_signal(m, MOUNT_UNMOUNTING_SIGTERM, false);
                break;

        case MOUNT_MOUNTING_SIGTERM:
                if (m->exec_context.send_sigkill) {
                        log_warning("%s mounting timed out. Killing.", u->meta.id);
                        mount_enter_signal(m, MOUNT_MOUNTING_SIGKILL, false);
                } else {
                        log_warning("%s mounting timed out. Skipping SIGKILL. Ignoring.", u->meta.id);

                        if (m->from_proc_self_mountinfo)
                                mount_enter_mounted(m, false);
                        else
                                mount_enter_dead(m, false);
                }
                break;

        case MOUNT_REMOUNTING_SIGTERM:
                if (m->exec_context.send_sigkill) {
                        log_warning("%s remounting timed out. Killing.", u->meta.id);
                        mount_enter_signal(m, MOUNT_REMOUNTING_SIGKILL, false);
                } else {
                        log_warning("%s remounting timed out. Skipping SIGKILL. Ignoring.", u->meta.id);

                        if (m->from_proc_self_mountinfo)
                                mount_enter_mounted(m, false);
                        else
                                mount_enter_dead(m, false);
                }
                break;

        case MOUNT_UNMOUNTING_SIGTERM:
                if (m->exec_context.send_sigkill) {
                        log_warning("%s unmounting timed out. Killing.", u->meta.id);
                        mount_enter_signal(m, MOUNT_UNMOUNTING_SIGKILL, false);
                } else {
                        log_warning("%s unmounting timed out. Skipping SIGKILL. Ignoring.", u->meta.id);

                        if (m->from_proc_self_mountinfo)
                                mount_enter_mounted(m, false);
                        else
                                mount_enter_dead(m, false);
                }
                break;

        case MOUNT_MOUNTING_SIGKILL:
        case MOUNT_REMOUNTING_SIGKILL:
        case MOUNT_UNMOUNTING_SIGKILL:
                log_warning("%s mount process still around after SIGKILL. Ignoring.", u->meta.id);

                if (m->from_proc_self_mountinfo)
                        mount_enter_mounted(m, false);
                else
                        mount_enter_dead(m, false);
                break;

        default:
                assert_not_reached("Timeout at wrong time.");
        }
}

static int mount_add_one(
                Manager *m,
                const char *what,
                const char *where,
                const char *options,
                const char *fstype,
                int passno,
                bool from_proc_self_mountinfo,
                bool set_flags) {
        int r;
        Unit *u;
        bool delete;
        char *e, *w = NULL, *o = NULL, *f = NULL;
        MountParameters *p;

        assert(m);
        assert(what);
        assert(where);
        assert(options);
        assert(fstype);

        assert(!set_flags || from_proc_self_mountinfo);

        /* Ignore API mount points. They should never be referenced in
         * dependencies ever. */
        if (mount_point_is_api(where) || mount_point_ignore(where))
                return 0;

        if (streq(fstype, "autofs"))
                return 0;

        /* probably some kind of swap, ignore */
        if (!is_path(where))
                return 0;

        if (!(e = unit_name_from_path(where, ".mount")))
                return -ENOMEM;

        if (!(u = manager_get_unit(m, e))) {
                delete = true;

                if (!(u = unit_new(m))) {
                        free(e);
                        return -ENOMEM;
                }

                r = unit_add_name(u, e);
                free(e);

                if (r < 0)
                        goto fail;

                if (!(MOUNT(u)->where = strdup(where))) {
                        r = -ENOMEM;
                        goto fail;
                }

                unit_add_to_load_queue(u);
        } else {
                delete = false;
                free(e);
        }

        if (!(w = strdup(what)) ||
            !(o = strdup(options)) ||
            !(f = strdup(fstype))) {
                r = -ENOMEM;
                goto fail;
        }

        if (from_proc_self_mountinfo) {
                p = &MOUNT(u)->parameters_proc_self_mountinfo;

                if (set_flags) {
                        MOUNT(u)->is_mounted = true;
                        MOUNT(u)->just_mounted = !MOUNT(u)->from_proc_self_mountinfo;
                        MOUNT(u)->just_changed = !streq_ptr(p->options, o);
                }

                MOUNT(u)->from_proc_self_mountinfo = true;
        } else {
                p = &MOUNT(u)->parameters_etc_fstab;
                MOUNT(u)->from_etc_fstab = true;
        }

        free(p->what);
        p->what = w;

        free(p->options);
        p->options = o;

        free(p->fstype);
        p->fstype = f;

        p->passno = passno;

        unit_add_to_dbus_queue(u);

        return 0;

fail:
        free(w);
        free(o);
        free(f);

        if (delete && u)
                unit_free(u);

        return r;
}

static int mount_find_pri(char *options) {
        char *end, *pri;
        unsigned long r;

        if (!(pri = mount_test_option(options, "pri=")))
                return 0;

        pri += 4;

        errno = 0;
        r = strtoul(pri, &end, 10);

        if (errno != 0)
                return -errno;

        if (end == pri || (*end != ',' && *end != 0))
                return -EINVAL;

        return (int) r;
}

static int mount_load_etc_fstab(Manager *m) {
        FILE *f;
        int r = 0;
        struct mntent* me;

        assert(m);

        errno = 0;
        if (!(f = setmntent("/etc/fstab", "r")))
                return -errno;

        while ((me = getmntent(f))) {
                char *where, *what;
                int k;

                if (!(what = fstab_node_to_udev_node(me->mnt_fsname))) {
                        r = -ENOMEM;
                        goto finish;
                }

                if (!(where = strdup(me->mnt_dir))) {
                        free(what);
                        r = -ENOMEM;
                        goto finish;
                }

                if (what[0] == '/')
                        path_kill_slashes(what);

                if (where[0] == '/')
                        path_kill_slashes(where);

                if (streq(me->mnt_type, "swap")) {
                        int pri;

                        if ((pri = mount_find_pri(me->mnt_opts)) < 0)
                                k = pri;
                        else
                                k = swap_add_one(m,
                                                 what,
                                                 NULL,
                                                 pri,
                                                 !!mount_test_option(me->mnt_opts, "noauto"),
                                                 !!mount_test_option(me->mnt_opts, "nofail"),
                                                 !!mount_test_option(me->mnt_opts, "comment=systemd.swapon"),
                                                 false);
                } else
                        k = mount_add_one(m, what, where, me->mnt_opts, me->mnt_type, me->mnt_passno, false, false);

                free(what);
                free(where);

                if (r < 0)
                        r = k;
        }

finish:

        endmntent(f);
        return r;
}

static int mount_load_proc_self_mountinfo(Manager *m, bool set_flags) {
        int r = 0;
        unsigned i;
        char *device, *path, *options, *options2, *fstype, *d, *p, *o;

        assert(m);

        rewind(m->proc_self_mountinfo);

        for (i = 1;; i++) {
                int k;

                device = path = options = options2 = fstype = d = p = o = NULL;

                if ((k = fscanf(m->proc_self_mountinfo,
                                "%*s "       /* (1) mount id */
                                "%*s "       /* (2) parent id */
                                "%*s "       /* (3) major:minor */
                                "%*s "       /* (4) root */
                                "%ms "       /* (5) mount point */
                                "%ms"        /* (6) mount options */
                                "%*[^-]"     /* (7) optional fields */
                                "- "         /* (8) separator */
                                "%ms "       /* (9) file system type */
                                "%ms"        /* (10) mount source */
                                "%ms"        /* (11) mount options 2 */
                                "%*[^\n]",   /* some rubbish at the end */
                                &path,
                                &options,
                                &fstype,
                                &device,
                                &options2)) != 5) {

                        if (k == EOF)
                                break;

                        log_warning("Failed to parse /proc/self/mountinfo:%u.", i);
                        goto clean_up;
                }

                if (asprintf(&o, "%s,%s", options, options2) < 0) {
                        r = -ENOMEM;
                        goto finish;
                }

                if (!(d = cunescape(device)) ||
                    !(p = cunescape(path))) {
                        r = -ENOMEM;
                        goto finish;
                }

                if ((k = mount_add_one(m, d, p, o, fstype, 0, true, set_flags)) < 0)
                        r = k;

clean_up:
                free(device);
                free(path);
                free(options);
                free(options2);
                free(fstype);
                free(d);
                free(p);
                free(o);
        }

finish:
        free(device);
        free(path);
        free(options);
        free(options2);
        free(fstype);
        free(d);
        free(p);
        free(o);

        return r;
}

static void mount_shutdown(Manager *m) {
        assert(m);

        if (m->proc_self_mountinfo) {
                fclose(m->proc_self_mountinfo);
                m->proc_self_mountinfo = NULL;
        }
}

static int mount_enumerate(Manager *m) {
        int r;
        struct epoll_event ev;
        assert(m);

        if (!m->proc_self_mountinfo) {
                if (!(m->proc_self_mountinfo = fopen("/proc/self/mountinfo", "re")))
                        return -errno;

                m->mount_watch.type = WATCH_MOUNT;
                m->mount_watch.fd = fileno(m->proc_self_mountinfo);

                zero(ev);
                ev.events = EPOLLPRI;
                ev.data.ptr = &m->mount_watch;

                if (epoll_ctl(m->epoll_fd, EPOLL_CTL_ADD, m->mount_watch.fd, &ev) < 0)
                        return -errno;
        }

        if ((r = mount_load_etc_fstab(m)) < 0)
                goto fail;

        if ((r = mount_load_proc_self_mountinfo(m, false)) < 0)
                goto fail;

        return 0;

fail:
        mount_shutdown(m);
        return r;
}

void mount_fd_event(Manager *m, int events) {
        Meta *meta;
        int r;

        assert(m);
        assert(events & EPOLLPRI);

        /* The manager calls this for every fd event happening on the
         * /proc/self/mountinfo file, which informs us about mounting
         * table changes */

        if ((r = mount_load_proc_self_mountinfo(m, true)) < 0) {
                log_error("Failed to reread /proc/self/mountinfo: %s", strerror(-r));

                /* Reset flags, just in case, for later calls */
                LIST_FOREACH(units_per_type, meta, m->units_per_type[UNIT_MOUNT]) {
                        Mount *mount = (Mount*) meta;

                        mount->is_mounted = mount->just_mounted = mount->just_changed = false;
                }

                return;
        }

        manager_dispatch_load_queue(m);

        LIST_FOREACH(units_per_type, meta, m->units_per_type[UNIT_MOUNT]) {
                Mount *mount = (Mount*) meta;

                if (!mount->is_mounted) {
                        /* This has just been unmounted. */

                        mount->from_proc_self_mountinfo = false;

                        switch (mount->state) {

                        case MOUNT_MOUNTED:
                                mount_enter_dead(mount, true);
                                break;

                        default:
                                mount_set_state(mount, mount->state);
                                break;

                        }

                } else if (mount->just_mounted || mount->just_changed) {

                        /* New or changed mount entry */

                        switch (mount->state) {

                        case MOUNT_DEAD:
                        case MOUNT_FAILED:
                                mount_enter_mounted(mount, true);
                                break;

                        case MOUNT_MOUNTING:
                                mount_enter_mounting_done(mount);
                                break;

                        default:
                                /* Nothing really changed, but let's
                                 * issue an notification call
                                 * nonetheless, in case somebody is
                                 * waiting for this. (e.g. file system
                                 * ro/rw remounts.) */
                                mount_set_state(mount, mount->state);
                                break;
                        }
                }

                /* Reset the flags for later calls */
                mount->is_mounted = mount->just_mounted = mount->just_changed = false;
        }
}

static void mount_reset_failed(Unit *u) {
        Mount *m = MOUNT(u);

        assert(m);

        if (m->state == MOUNT_FAILED)
                mount_set_state(m, MOUNT_DEAD);

        m->failure = false;
}

static int mount_kill(Unit *u, KillWho who, KillMode mode, int signo, DBusError *error) {
        Mount *m = MOUNT(u);
        int r = 0;
        Set *pid_set = NULL;

        assert(m);

        if (who == KILL_MAIN) {
                dbus_set_error(error, BUS_ERROR_NO_SUCH_PROCESS, "Mount units have no main processes");
                return -EINVAL;
        }

        if (m->control_pid <= 0 && who == KILL_CONTROL) {
                dbus_set_error(error, BUS_ERROR_NO_SUCH_PROCESS, "No control process to kill");
                return -ENOENT;
        }

        if (m->control_pid > 0)
                if (kill(m->control_pid, signo) < 0)
                        r = -errno;

        if (mode == KILL_CONTROL_GROUP) {
                int q;

                if (!(pid_set = set_new(trivial_hash_func, trivial_compare_func)))
                        return -ENOMEM;

                /* Exclude the control pid from being killed via the cgroup */
                if (m->control_pid > 0)
                        if ((q = set_put(pid_set, LONG_TO_PTR(m->control_pid))) < 0) {
                                r = q;
                                goto finish;
                        }

                if ((q = cgroup_bonding_kill_list(m->meta.cgroup_bondings, signo, false, pid_set)) < 0)
                        if (r != -EAGAIN && r != -ESRCH && r != -ENOENT)
                                r = q;
        }

finish:
        if (pid_set)
                set_free(pid_set);

        return r;
}

static const char* const mount_state_table[_MOUNT_STATE_MAX] = {
        [MOUNT_DEAD] = "dead",
        [MOUNT_MOUNTING] = "mounting",
        [MOUNT_MOUNTING_DONE] = "mounting-done",
        [MOUNT_MOUNTED] = "mounted",
        [MOUNT_REMOUNTING] = "remounting",
        [MOUNT_UNMOUNTING] = "unmounting",
        [MOUNT_MOUNTING_SIGTERM] = "mounting-sigterm",
        [MOUNT_MOUNTING_SIGKILL] = "mounting-sigkill",
        [MOUNT_REMOUNTING_SIGTERM] = "remounting-sigterm",
        [MOUNT_REMOUNTING_SIGKILL] = "remounting-sigkill",
        [MOUNT_UNMOUNTING_SIGTERM] = "unmounting-sigterm",
        [MOUNT_UNMOUNTING_SIGKILL] = "unmounting-sigkill",
        [MOUNT_FAILED] = "failed"
};

DEFINE_STRING_TABLE_LOOKUP(mount_state, MountState);

static const char* const mount_exec_command_table[_MOUNT_EXEC_COMMAND_MAX] = {
        [MOUNT_EXEC_MOUNT] = "ExecMount",
        [MOUNT_EXEC_UNMOUNT] = "ExecUnmount",
        [MOUNT_EXEC_REMOUNT] = "ExecRemount",
};

DEFINE_STRING_TABLE_LOOKUP(mount_exec_command, MountExecCommand);

const UnitVTable mount_vtable = {
        .suffix = ".mount",

        .no_alias = true,
        .no_instances = true,
        .show_status = true,

        .init = mount_init,
        .load = mount_load,
        .done = mount_done,

        .coldplug = mount_coldplug,

        .dump = mount_dump,

        .start = mount_start,
        .stop = mount_stop,
        .reload = mount_reload,

        .kill = mount_kill,

        .serialize = mount_serialize,
        .deserialize_item = mount_deserialize_item,

        .active_state = mount_active_state,
        .sub_state_to_string = mount_sub_state_to_string,

        .check_gc = mount_check_gc,

        .sigchld_event = mount_sigchld_event,
        .timer_event = mount_timer_event,

        .reset_failed = mount_reset_failed,

        .bus_interface = "org.freedesktop.systemd1.Mount",
        .bus_message_handler = bus_mount_message_handler,
        .bus_invalidating_properties =  bus_mount_invalidating_properties,

        .enumerate = mount_enumerate,
        .shutdown = mount_shutdown
};
