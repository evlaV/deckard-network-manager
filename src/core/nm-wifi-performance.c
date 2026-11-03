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

#define _NMLOG_PREFIX_NAME "wifi-perf"
#define _NMLOG_DOMAIN      LOGD_WIFI
#define _NMLOG(level, ...)                                                 \
    G_STMT_START                                                           \
    {                                                                      \
        nm_log((level),                                                    \
               _NMLOG_DOMAIN,                                              \
               NULL,                                                       \
               NULL,                                                       \
               "%s" _NM_UTILS_MACRO_FIRST(__VA_ARGS__),                    \
               _NMLOG_PREFIX_NAME ": " _NM_UTILS_MACRO_REST(__VA_ARGS__)); \
    }                                                                      \
    G_STMT_END

/*****************************************************************************/

typedef struct {
    CList              lst;
    NMWifiPerformance *self;
    /* D-Bus unique name of the holder (":1.42"). There are only ever a few
     * leases, so a list with string compares beats translating to a non-string
     * representation. */
    char                 *dbus_owner;
    char                 *reason;
    NMWifiPerformanceMode mode;

    /* Watch for the holder leaving the bus. */
    GDBusConnection *dbus_connection;
    /* Cancels the pending name owner lookup that checks the holder is still
     * on the bus when the lease is taken. */
    GCancellable *name_owner_cancellable;
    /* Subscription ID for NameOwnerChanged on dbus_owner, which is used to drop the
     * lease when the holder leaves the bus. 0 if not subscribed. */
    guint name_owner_changed_id;
} WifiPerformanceLease;

enum {
    LEASES_CHANGED,
    LAST_SIGNAL,
};

static guint signals[LAST_SIGNAL] = {0};

struct _NMWifiPerformance {
    GObject parent;
    /* WifiPerformanceLease.lst, one per holder */
    CList leases_lst_head;
};

struct _NMWifiPerformanceClass {
    GObjectClass parent;
};

G_DEFINE_TYPE(NMWifiPerformance, nm_wifi_performance, G_TYPE_OBJECT)

NM_DEFINE_SINGLETON_GETTER(NMWifiPerformance, nm_wifi_performance_get, NM_TYPE_WIFI_PERFORMANCE);

/*****************************************************************************/

/* The names used on D-Bus and in the saved leases. */
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

/*****************************************************************************/

/* One keyfile for all leases. /run survives an NM restart but not a reboot,
 * which is the lifetime of a lease: every holder is gone after a reboot. NM
 * only reads this file at startup. */
#define WIFI_PERFORMANCE_STATE_FILE         NMRUNDIR "/wifi-performance-leases"
#define WIFI_PERFORMANCE_STATE_GROUP_PREFIX "lease "
#define WIFI_PERFORMANCE_STATE_KEY_REASON   "reason"
#define WIFI_PERFORMANCE_STATE_KEY_MODE     "mode"

/* Holders are saved by unique name, which the bus never reuses while it
 * runs, but a restarted bus numbers names from the start again. So the file
 * records which bus instance the names belong to, and leases saved on
 * another bus are discarded instead of being matched to unrelated
 * processes. This is the GUID the bus sent when NM connected, which is new
 * for every bus instance. On dbus-broker it is not the same string as
 * org.freedesktop.DBus.GetId; only compare it with itself. */
#define WIFI_PERFORMANCE_STATE_GROUP_BUS  "bus"
#define WIFI_PERFORMANCE_STATE_KEY_BUS_ID "id"

static const char *
_state_bus_id(void)
{
    GDBusConnection *connection = NM_MAIN_DBUS_CONNECTION_GET;

    return connection ? g_dbus_connection_get_guid(connection) : NULL;
}

/* Saves the leases under /run so a restarted NM can restore them without
 * holders needing to handle the restart. Called after every change.
 * Returns FALSE if the file could not be updated. */
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

    /* Written to a temporary file and renamed, so a crash leaves either the
     * old or the new set of leases, never a partial file. The file is updated
     * before success is returned to the caller of AcquireWifiPerformanceLease(),
     * so a restart that results in the old list being loaded should not lose active
     * leases, as those callers are returned an error. */
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

/* Tells the Wi-Fi devices if the set of modes held changed. */
static void
wifi_performance_notify(NMWifiPerformance *self, NMWifiPerformanceMode old_modes)
{
    if (old_modes != nm_wifi_performance_get_modes(self))
        g_signal_emit(self, signals[LEASES_CHANGED], 0);
}

/* Saves the leases and notifies the Wi-Fi devices. */
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

    _LOGI("%s lease of %s (\"%s\") dropped: %s",
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

    wifi_performance_lease_lost(lease, "holder left the bus");
}

static void
wifi_performance_get_name_owner_cb(const char *name_owner, GError *error, gpointer user_data)
{
    WifiPerformanceLease *lease;

    /* Cancelled because the lease was removed (it is already freed). */
    if (!name_owner && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

    lease = user_data;
    g_clear_object(&lease->name_owner_cancellable);

    if (nm_streq0(name_owner, lease->dbus_owner))
        return; /* Still there. */

    wifi_performance_lease_lost(lease, "holder left the bus before the watch was set up");
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

    /** If the holder disconnects from the bus after its Acquire() call but before
     * NM subscribes to NameOwnerChanged for it, the bus may emit the signal before
     * the match rule exists.
     *
     * To prevent the lease persisting after its holder disconnected, subscribe first
     * and only then ask the bus for the current owner. The bus processes the messages
     * in order, so the match rule will be installed before GetNameOwner() is answered.
     *
     * Two cases:
     *
     *   1. if the holder disconnects after the match rule is installed, we get
     *      NameOwnerChanged, and wifi_performance_owner_changed_cb() drops the
     *      lease (which cancels the pending GetNameOwner() call);
     *   2. if the holder disconnected before, no signal arrives, but GetNameOwner()
     *      reports no (or a different) owner and wifi_performance_get_name_owner_cb()
     *      drops the lease. */
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

    /* One lease per holder. Acquiring the same mode again does nothing. */
    lease = wifi_performance_find_lease(self, sender);
    if (lease) {
        if (lease->mode == mode)
            return TRUE;
        _LOGI("%s lease for %s refused: holds a %s lease",
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

    /* A lease that isn't saved would be lost if NM restarted, so it is only
     * granted once it is saved. The rename keeps the old file intact on
     * failure, so removing the lease is all that is needed to undo it. */
    if (!wifi_performance_save(self)) {
        wifi_performance_remove_lease(lease);
        g_set_error_literal(error,
                            NM_MANAGER_ERROR,
                            NM_MANAGER_ERROR_FAILED,
                            "Could not save the performance lease");
        return FALSE;
    }

    _LOGI("%s lease acquired by %s (\"%s\")",
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

    /* Releasing is idempotent: a holder that is unsure whether an earlier
     * Release went through (e.g. NM restarted before replying) can call it
     * again until it succeeds. */
    lease = wifi_performance_find_lease(self, sender);
    if (!lease) {
        _LOGI("release by %s: holds no lease, nothing to do", sender);
        return TRUE;
    }

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

    _LOGI("%s lease released by %s (\"%s\")",
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

    /* No D-Bus (initrd): nobody can hold a lease. */
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

    /* A file without a bus ID can't be checked, so it is discarded too. */
    saved_bus_id = g_key_file_get_string(kf,
                                         WIFI_PERFORMANCE_STATE_GROUP_BUS,
                                         WIFI_PERFORMANCE_STATE_KEY_BUS_ID,
                                         NULL);

    /* D-Bus holder names are unique to a D-Bus instance.
     * Check the bus_id to prevent the case where D-Bus restarts and new holders are assigned
     * the unique names of previous active lease owners. */
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
        const char           *holder;

        if (!NM_STR_HAS_PREFIX(groups[i], WIFI_PERFORMANCE_STATE_GROUP_PREFIX))
            continue;

        holder = &groups[i][NM_STRLEN(WIFI_PERFORMANCE_STATE_GROUP_PREFIX)];
        if (!g_dbus_is_unique_name(holder)) {
            _LOGW("ignoring saved lease of invalid holder \"%s\"", holder);
            continue;
        }
        if (wifi_performance_find_lease(self, holder))
            continue;

        mode_str = g_key_file_get_string(kf, groups[i], WIFI_PERFORMANCE_STATE_KEY_MODE, NULL);
        mode     = nm_wifi_performance_mode_from_string(mode_str);
        if (mode == NM_WIFI_PERFORMANCE_MODE_NONE) {
            _LOGW("ignoring saved lease of %s with unknown mode \"%s\"",
                  holder,
                  mode_str ?: "(none)");
            continue;
        }

        reason = g_key_file_get_string(kf, groups[i], WIFI_PERFORMANCE_STATE_KEY_REASON, NULL);
        _LOGI("%s lease of %s (\"%s\") restored",
              wifi_performance_mode_to_string(mode),
              holder,
              reason ?: "");
        wifi_performance_add_lease(self, connection, holder, mode, reason ?: "");
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

    /* Frees the leases but keeps the saved file, which a restarted NM
     * reads back. */
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
