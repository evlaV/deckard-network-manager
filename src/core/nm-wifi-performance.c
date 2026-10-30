/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "src/core/nm-default-daemon.h"

#include "nm-wifi-performance.h"

#include <unistd.h>

#include "libnm-glib-aux/nm-c-list.h"
#include "libnm-glib-aux/nm-dbus-aux.h"
#include "libnm-glib-aux/nm-io-utils.h"
#include "nm-config.h"
#include "nm-core-utils.h"
#include "nm-dbus-manager.h"

#define _NMLOG_DOMAIN      LOGD_WIFI
#define _NMLOG(level, ...) __NMLOG_DEFAULT(level, _NMLOG_DOMAIN, "wifi-perf", __VA_ARGS__)

/*****************************************************************************/

typedef struct {
    CList                 lst;
    NMWifiPerformance    *self;
    char                 *dbus_owner;
    char                 *reason;
    NMWifiPerformanceMode mode;

    GDBusConnection *dbus_connection;
    GCancellable    *name_owner_cancellable;
    guint            name_owner_changed_id;
} WifiPerformanceLease;

enum {
    LEASES_CHANGED,
    LAST_SIGNAL,
};

static guint signals[LAST_SIGNAL] = {0};

struct _NMWifiPerformance {
    GObject parent;
    CList   leases_lst_head;
};

struct _NMWifiPerformanceClass {
    GObjectClass parent;
};

G_DEFINE_TYPE(NMWifiPerformance, nm_wifi_performance, G_TYPE_OBJECT)

NM_DEFINE_SINGLETON_GETTER(NMWifiPerformance, nm_wifi_performance_get, NM_TYPE_WIFI_PERFORMANCE);

/*****************************************************************************/

/* These names are used on D-Bus and in the state file. Don't change them, and
 * keep nm_wifi_performance_mode_from_string() in sync. */
static NM_UTILS_LOOKUP_STR_DEFINE(wifi_performance_mode_to_string,
                                  NMWifiPerformanceMode,
                                  NM_UTILS_LOOKUP_DEFAULT_NM_ASSERT("unknown"),
                                  NM_UTILS_LOOKUP_STR_ITEM(NM_WIFI_PERFORMANCE_MODE_INTERACTIVE,
                                                           "interactive"),
                                  NM_UTILS_LOOKUP_ITEM_IGNORE(NM_WIFI_PERFORMANCE_MODE_NONE), );

NMWifiPerformanceMode
nm_wifi_performance_mode_from_string(const char *str)
{
    if (nm_streq0(str, "interactive"))
        return NM_WIFI_PERFORMANCE_MODE_INTERACTIVE;
    return NM_WIFI_PERFORMANCE_MODE_NONE;
}

/* Reject control characters to ensure reason is trivially parseable.
 * Limit string length to bound memory usage. */
gboolean
nm_wifi_performance_reason_is_valid(const char *reason, GError **error)
{
    const char *p;

    g_return_val_if_fail(reason, FALSE);

    if (strlen(reason) > NM_WIFI_PERFORMANCE_REASON_MAX_LEN_BYTES) {
        g_set_error(error,
                    NM_MANAGER_ERROR,
                    NM_MANAGER_ERROR_INVALID_ARGUMENTS,
                    "Reason is longer than %d bytes",
                    NM_WIFI_PERFORMANCE_REASON_MAX_LEN_BYTES);
        return FALSE;
    }
    if (!g_utf8_validate(reason, -1, NULL)) {
        g_set_error_literal(error,
                            NM_MANAGER_ERROR,
                            NM_MANAGER_ERROR_INVALID_ARGUMENTS,
                            "Reason is not valid UTF-8");
        return FALSE;
    }
    for (p = reason; *p; p = g_utf8_next_char(p)) {
        if (g_unichar_iscntrl(g_utf8_get_char(p))) {
            g_set_error_literal(error,
                                NM_MANAGER_ERROR,
                                NM_MANAGER_ERROR_INVALID_ARGUMENTS,
                                "Reason contains a control character");
            return FALSE;
        }
    }
    return TRUE;
}

/*****************************************************************************/

/* We save the leases here and restore them when NM restarts. They can't
 * outlive a reboot anyway, hence /run. */
#define WIFI_PERFORMANCE_STATE_FILE         NMRUNDIR "/wifi-performance-leases"
#define WIFI_PERFORMANCE_STATE_GROUP_PREFIX "lease "
#define WIFI_PERFORMANCE_STATE_KEY_REASON   "reason"
#define WIFI_PERFORMANCE_STATE_KEY_MODE     "mode"
#define WIFI_PERFORMANCE_STATE_GROUP_BUS    "bus"
#define WIFI_PERFORMANCE_STATE_KEY_BUS_ID   "id"

/* The GUID the bus sent us during authentication. It's not the ID that
 * GetId() returns, but it also changes every time the bus restarts, and
 * we have it without making a D-Bus call. */
static const char *
_state_bus_id(void)
{
    GDBusConnection *connection = NM_MAIN_DBUS_CONNECTION_GET;

    return connection ? g_dbus_connection_get_guid(connection) : NULL;
}

/* This updates the file atomically and synchronously.
 * nm_wifi_performance_acquire() relies on both. */
static gboolean
wifi_performance_save(NMWifiPerformance *self)
{
    nm_auto_unref_keyfile GKeyFile *kf    = NULL;
    gs_free char                   *data  = NULL;
    gs_free_error GError           *error = NULL;
    WifiPerformanceLease           *lease;
    const char                     *bus_id;
    gsize                           len;
    int                             errsv;

    if (c_list_is_empty(&self->leases_lst_head)) {
        if (unlink(WIFI_PERFORMANCE_STATE_FILE) != 0 && (errsv = errno) != ENOENT) {
            _LOGW("removing saved leases %s failed: %s",
                  WIFI_PERFORMANCE_STATE_FILE,
                  nm_strerror_native(errsv));
            return FALSE;
        }
        _LOGT("no leases left, removed %s", WIFI_PERFORMANCE_STATE_FILE);
        return TRUE;
    }

    kf     = nm_config_create_keyfile();
    bus_id = _state_bus_id();
    if (bus_id) {
        g_key_file_set_string(kf,
                              WIFI_PERFORMANCE_STATE_GROUP_BUS,
                              WIFI_PERFORMANCE_STATE_KEY_BUS_ID,
                              bus_id);
    }
    c_list_for_each_entry (lease, &self->leases_lst_head, lst) {
        gs_free char *group = NULL;

        group = g_strconcat(WIFI_PERFORMANCE_STATE_GROUP_PREFIX, lease->dbus_owner, NULL);
        g_key_file_set_string(kf,
                              group,
                              WIFI_PERFORMANCE_STATE_KEY_MODE,
                              wifi_performance_mode_to_string(lease->mode));
        g_key_file_set_string(kf, group, WIFI_PERFORMANCE_STATE_KEY_REASON, lease->reason);
    }
    data = g_key_file_to_data(kf, &len, NULL);

    if (!nm_utils_file_set_contents(WIFI_PERFORMANCE_STATE_FILE,
                                    data,
                                    len,
                                    0600,
                                    NULL,
                                    NULL,
                                    &error)) {
        _LOGW("saving leases to %s failed: %s", WIFI_PERFORMANCE_STATE_FILE, error->message);
        return FALSE;
    }
    _LOGT("saved %zu lease(s) to %s",
          c_list_length(&self->leases_lst_head),
          WIFI_PERFORMANCE_STATE_FILE);
    return TRUE;
}

/*****************************************************************************/

NMWifiPerformanceMode
nm_wifi_performance_get_modes(NMWifiPerformance *self)
{
    NMWifiPerformanceMode modes = NM_WIFI_PERFORMANCE_MODE_NONE;
    WifiPerformanceLease *lease;

    g_return_val_if_fail(NM_IS_WIFI_PERFORMANCE(self), NM_WIFI_PERFORMANCE_MODE_NONE);

    c_list_for_each_entry (lease, &self->leases_lst_head, lst)
        modes |= lease->mode;
    return modes;
}

static void
wifi_performance_notify(NMWifiPerformance *self, NMWifiPerformanceMode old_modes)
{
    if (old_modes != nm_wifi_performance_get_modes(self))
        g_signal_emit(self, signals[LEASES_CHANGED], 0);
}

static void
wifi_performance_changed(NMWifiPerformance *self, NMWifiPerformanceMode old_modes)
{
    wifi_performance_save(self);
    wifi_performance_notify(self, old_modes);
}

static void
wifi_performance_remove_lease(WifiPerformanceLease *lease)
{
    nm_assert(lease);

    c_list_unlink(&lease->lst);
    nm_clear_g_cancellable(&lease->name_owner_cancellable);
    nm_clear_g_dbus_connection_signal(lease->dbus_connection, &lease->name_owner_changed_id);
    g_clear_object(&lease->dbus_connection);
    g_free(lease->dbus_owner);
    g_free(lease->reason);
    g_free(lease);
}

static void
wifi_performance_lease_lost(WifiPerformanceLease *lease, const char *why)
{
    NMWifiPerformance    *self = lease->self;
    NMWifiPerformanceMode old_modes;

    _LOGI("%s performance lease of %s (\"%s\") dropped: %s",
          wifi_performance_mode_to_string(lease->mode),
          lease->dbus_owner,
          lease->reason,
          why);
    old_modes = nm_wifi_performance_get_modes(self);
    wifi_performance_remove_lease(lease);
    wifi_performance_changed(self, old_modes);
}

static void
wifi_performance_owner_changed_cb(GDBusConnection *connection,
                                  const char      *sender_name,
                                  const char      *object_path,
                                  const char      *interface_name,
                                  const char      *signal_name,
                                  GVariant        *parameters,
                                  gpointer         user_data)
{
    WifiPerformanceLease *lease = user_data;
    const char           *new_owner;

    g_variant_get(parameters, "(&s&s&s)", NULL, NULL, &new_owner);

    if (new_owner[0] != '\0')
        return;

    wifi_performance_lease_lost(lease, "client left the bus");
}

static void
wifi_performance_get_name_owner_cb(const char *name_owner, GError *error, gpointer user_data)
{
    WifiPerformanceLease *lease;

    if (!name_owner && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

    lease = user_data;
    g_clear_object(&lease->name_owner_cancellable);

    if (nm_streq0(name_owner, lease->dbus_owner))
        return; /* still on the bus */

    wifi_performance_lease_lost(lease, "client left the bus before the watch was set up");
}

static WifiPerformanceLease *
wifi_performance_add_lease(NMWifiPerformance    *self,
                           GDBusConnection      *connection,
                           const char           *sender,
                           NMWifiPerformanceMode mode,
                           const char           *reason)
{
    WifiPerformanceLease *lease;

    nm_assert(connection);
    nm_assert(sender);
    nm_assert(mode != NM_WIFI_PERFORMANCE_MODE_NONE);
    nm_assert(reason);

    lease                  = g_new0(WifiPerformanceLease, 1);
    lease->self            = self;
    lease->dbus_connection = g_object_ref(connection);
    lease->dbus_owner      = g_strdup(sender);
    lease->reason          = g_strdup(reason);
    lease->mode            = mode;
    c_list_link_tail(&self->leases_lst_head, &lease->lst);

    /* Subscribe before calling GetNameOwner(). The bus handles both messages in
     * order. If the client left before the match rule was in place, including
     * while NM was not running for a restored lease, GetNameOwner() catches it. */
    lease->name_owner_changed_id =
        nm_dbus_connection_signal_subscribe_name_owner_changed(connection,
                                                               lease->dbus_owner,
                                                               wifi_performance_owner_changed_cb,
                                                               lease,
                                                               NULL);
    lease->name_owner_cancellable = g_cancellable_new();
    nm_dbus_connection_call_get_name_owner(connection,
                                           lease->dbus_owner,
                                           -1,
                                           lease->name_owner_cancellable,
                                           wifi_performance_get_name_owner_cb,
                                           lease);
    return lease;
}

static WifiPerformanceLease *
wifi_performance_find_lease(NMWifiPerformance *self, const char *sender)
{
    WifiPerformanceLease *lease;

    nm_assert(sender);

    c_list_for_each_entry (lease, &self->leases_lst_head, lst) {
        if (nm_streq(lease->dbus_owner, sender))
            return lease;
    }
    return NULL;
}

gboolean
nm_wifi_performance_acquire(NMWifiPerformance    *self,
                            GDBusConnection      *connection,
                            const char           *sender,
                            NMWifiPerformanceMode mode,
                            const char           *reason,
                            GError              **error)
{
    WifiPerformanceLease *lease;
    NMWifiPerformanceMode old_modes;

    g_return_val_if_fail(NM_IS_WIFI_PERFORMANCE(self), FALSE);
    g_return_val_if_fail(connection, FALSE);
    g_return_val_if_fail(sender, FALSE);
    g_return_val_if_fail(mode != NM_WIFI_PERFORMANCE_MODE_NONE, FALSE);
    g_return_val_if_fail(reason, FALSE);
    g_return_val_if_fail(!error || !*error, FALSE);

    /* One lease per caller. Requesting the same mode again is a no-op. */
    lease = wifi_performance_find_lease(self, sender);
    if (lease) {
        if (lease->mode == mode)
            return TRUE;
        _LOGI("%s performance lease for %s refused: holds a %s lease",
              wifi_performance_mode_to_string(mode),
              sender,
              wifi_performance_mode_to_string(lease->mode));
        g_set_error_literal(error,
                            NM_MANAGER_ERROR,
                            NM_MANAGER_ERROR_ALREADY_ENABLED_OR_DISABLED,
                            "The caller already holds a performance lease in another mode");
        return FALSE;
    }

    old_modes = nm_wifi_performance_get_modes(self);
    lease     = wifi_performance_add_lease(self, connection, sender, mode, reason);

    /* Only grant the lease after saving it. Otherwise we would lose it on a
     * restart of NM. On failure the old file is untouched, and removing the
     * lease from the list is enough to undo it. */
    if (!wifi_performance_save(self)) {
        wifi_performance_remove_lease(lease);
        g_set_error_literal(error,
                            NM_MANAGER_ERROR,
                            NM_MANAGER_ERROR_FAILED,
                            "Could not save the performance lease");
        return FALSE;
    }

    _LOGI("%s performance lease acquired by %s (\"%s\")",
          wifi_performance_mode_to_string(mode),
          sender,
          reason);
    wifi_performance_notify(self, old_modes);
    return TRUE;
}

gboolean
nm_wifi_performance_release(NMWifiPerformance *self, const char *sender, GError **error)
{
    WifiPerformanceLease *lease;
    NMWifiPerformanceMode old_modes;
    CList                *next;

    g_return_val_if_fail(NM_IS_WIFI_PERFORMANCE(self), FALSE);
    g_return_val_if_fail(sender, FALSE);
    g_return_val_if_fail(!error || !*error, FALSE);

    /* Not holding a lease is not an error. A client that doesn't know whether
     * an earlier release went through, for example because NM restarted
     * before replying, can retry until it succeeds. */
    lease = wifi_performance_find_lease(self, sender);
    if (!lease) {
        _LOGI("release by %s: holds no performance lease, nothing to do", sender);
        return TRUE;
    }

    /* Only drop the lease after saving the file without it. Otherwise NM
     * would restore it after a restart, and the client would hold it until it
     * leaves the bus. On failure, put the lease back where it was. */
    old_modes = nm_wifi_performance_get_modes(self);
    next      = lease->lst.next;
    c_list_unlink(&lease->lst);
    if (!wifi_performance_save(self)) {
        c_list_link_before(next, &lease->lst);
        g_set_error_literal(error,
                            NM_MANAGER_ERROR,
                            NM_MANAGER_ERROR_FAILED,
                            "Could not save the release of the performance lease");
        return FALSE;
    }

    _LOGI("%s performance lease released by %s (\"%s\")",
          wifi_performance_mode_to_string(lease->mode),
          sender,
          lease->reason);
    wifi_performance_remove_lease(lease);
    wifi_performance_notify(self, old_modes);
    return TRUE;
}

/*****************************************************************************/

void
nm_wifi_performance_restore(NMWifiPerformance *self)
{
    nm_auto_unref_keyfile GKeyFile *kf           = NULL;
    gs_strfreev char              **groups       = NULL;
    gs_free char                   *saved_bus_id = NULL;
    gs_free_error GError           *error        = NULL;
    GDBusConnection                *connection   = NM_MAIN_DBUS_CONNECTION_GET;
    const char                     *bus_id;
    NMWifiPerformanceMode           old_modes;
    guint                           i;

    g_return_if_fail(NM_IS_WIFI_PERFORMANCE(self));

    /* In initrd there is no D-Bus, and nobody can hold a lease */
    if (!connection)
        return;

    kf = nm_config_create_keyfile();
    if (!g_key_file_load_from_file(kf, WIFI_PERFORMANCE_STATE_FILE, G_KEY_FILE_NONE, &error)) {
        if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            _LOGW("reading saved leases %s failed: %s",
                  WIFI_PERFORMANCE_STATE_FILE,
                  error->message);
        return;
    }

    saved_bus_id = g_key_file_get_string(kf,
                                         WIFI_PERFORMANCE_STATE_GROUP_BUS,
                                         WIFI_PERFORMANCE_STATE_KEY_BUS_ID,
                                         NULL);

    /* Unique names are only unique for the lifetime of the bus. If the bus
     * was restarted, the saved names might now belong to unrelated clients
     * and we must drop the leases. The same applies if the file has no bus
     * ID, since we can't check it. */
    bus_id = _state_bus_id();
    if (!bus_id || !nm_streq0(saved_bus_id, bus_id)) {
        _LOGI("discarding saved leases: saved on bus %s, now on bus %s",
              saved_bus_id ?: "(unknown)",
              bus_id ?: "(none)");
        (void) unlink(WIFI_PERFORMANCE_STATE_FILE);
        return;
    }

    old_modes = nm_wifi_performance_get_modes(self);
    groups    = g_key_file_get_groups(kf, NULL);
    for (i = 0; groups[i]; i++) {
        gs_free char         *reason   = NULL;
        gs_free char         *mode_str = NULL;
        NMWifiPerformanceMode mode;
        const char           *dbus_owner;

        if (!NM_STR_HAS_PREFIX(groups[i], WIFI_PERFORMANCE_STATE_GROUP_PREFIX))
            continue;

        dbus_owner = &groups[i][NM_STRLEN(WIFI_PERFORMANCE_STATE_GROUP_PREFIX)];
        if (!g_dbus_is_unique_name(dbus_owner)) {
            _LOGW("ignoring saved performance lease of invalid name \"%s\"", dbus_owner);
            continue;
        }
        if (wifi_performance_find_lease(self, dbus_owner))
            continue;

        mode_str = g_key_file_get_string(kf, groups[i], WIFI_PERFORMANCE_STATE_KEY_MODE, NULL);
        mode     = nm_wifi_performance_mode_from_string(mode_str);
        if (mode == NM_WIFI_PERFORMANCE_MODE_NONE) {
            _LOGW("ignoring saved performance lease of %s with unknown mode \"%s\"",
                  dbus_owner,
                  mode_str ?: "(none)");
            continue;
        }

        reason = g_key_file_get_string(kf, groups[i], WIFI_PERFORMANCE_STATE_KEY_REASON, NULL);
        if (reason && !nm_wifi_performance_reason_is_valid(reason, NULL)) {
            _LOGW("ignoring saved performance lease of %s with an invalid reason", dbus_owner);
            continue;
        }
        _LOGI("%s performance lease of %s (\"%s\") restored",
              wifi_performance_mode_to_string(mode),
              dbus_owner,
              reason ?: "");
        wifi_performance_add_lease(self, connection, dbus_owner, mode, reason ?: "");
    }
    wifi_performance_changed(self, old_modes);
}

/*****************************************************************************/

static void
nm_wifi_performance_init(NMWifiPerformance *self)
{
    c_list_init(&self->leases_lst_head);
}

static void
dispose(GObject *object)
{
    NMWifiPerformance    *self = NM_WIFI_PERFORMANCE(object);
    WifiPerformanceLease *lease;
    WifiPerformanceLease *lease_safe;

    /* Don't delete the file. We restore the leases from it after a restart. */
    c_list_for_each_entry_safe (lease, lease_safe, &self->leases_lst_head, lst)
        wifi_performance_remove_lease(lease);

    G_OBJECT_CLASS(nm_wifi_performance_parent_class)->dispose(object);
}

static void
nm_wifi_performance_class_init(NMWifiPerformanceClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = dispose;

    signals[LEASES_CHANGED] = g_signal_new(NM_WIFI_PERFORMANCE_LEASES_CHANGED,
                                           G_OBJECT_CLASS_TYPE(object_class),
                                           G_SIGNAL_RUN_FIRST,
                                           0,
                                           NULL,
                                           NULL,
                                           NULL,
                                           G_TYPE_NONE,
                                           0);
}

GVariant *
nm_wifi_performance_get_leases(NMWifiPerformance *self)
{
    NMDBusManager        *dbus_manager = nm_dbus_manager_get();
    GVariantBuilder       builder;
    WifiPerformanceLease *lease;

    g_return_val_if_fail(NM_IS_WIFI_PERFORMANCE(self), NULL);

    g_variant_builder_init(&builder, G_VARIANT_TYPE("a(sssuu)"));
    c_list_for_each_entry (lease, &self->leases_lst_head, lst) {
        gulong uid = G_MAXULONG;
        gulong pid = G_MAXULONG;

        nm_dbus_manager_get_unix_user(dbus_manager, lease->dbus_owner, &uid);
        nm_dbus_manager_get_unix_pid(dbus_manager, lease->dbus_owner, &pid);
        g_variant_builder_add(&builder,
                              "(sssuu)",
                              wifi_performance_mode_to_string(lease->mode),
                              lease->reason,
                              lease->dbus_owner,
                              (guint32) uid,
                              (guint32) pid);
    }
    return g_variant_builder_end(&builder);
}
