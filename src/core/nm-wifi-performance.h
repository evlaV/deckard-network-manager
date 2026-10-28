/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef __NM_WIFI_PERFORMANCE_H__
#define __NM_WIFI_PERFORMANCE_H__

/* Wi-Fi performance leases. A client asks for Wi-Fi tuned for its kind of
 * traffic with AcquireWifiPerformanceLease on the Manager, naming a mode.
 * Devices are responsible for resolving the modes requested by lease holders.
 *
 * The leases belong to NetworkManager and not to a device, meaning they outlive
 * a device being recreated under a new object path. Leases also persist across
 * restarts of NetworkManager. Leases do not survive process death of the lease
 * holder, the holder disconnecting from the bus or a reset of D-Bus. */

/* Flags, so a set of modes held at once fits in one value; a single lease
 * has exactly one mode. */
typedef enum {
    NM_WIFI_PERFORMANCE_MODE_NONE = 0,

    /* Interactive traffic (game streaming, ssh), as opposed to bulk
     * transfers: power save stays off on every Wi-Fi client connection. */
    NM_WIFI_PERFORMANCE_MODE_INTERACTIVE = (1u << 0),
} NMWifiPerformanceMode;

#define NM_TYPE_WIFI_PERFORMANCE (nm_wifi_performance_get_type())
#define NM_WIFI_PERFORMANCE(obj) \
    (_NM_G_TYPE_CHECK_INSTANCE_CAST((obj), NM_TYPE_WIFI_PERFORMANCE, NMWifiPerformance))
#define NM_WIFI_PERFORMANCE_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass), NM_TYPE_WIFI_PERFORMANCE, NMWifiPerformanceClass))
#define NM_IS_WIFI_PERFORMANCE(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), NM_TYPE_WIFI_PERFORMANCE))
#define NM_IS_WIFI_PERFORMANCE_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_TYPE((klass), NM_TYPE_WIFI_PERFORMANCE))
#define NM_WIFI_PERFORMANCE_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS((obj), NM_TYPE_WIFI_PERFORMANCE, NMWifiPerformanceClass))

/* Emitted when the set of modes held changes. */
#define NM_WIFI_PERFORMANCE_LEASES_CHANGED "leases-changed"

typedef struct _NMWifiPerformance      NMWifiPerformance;
typedef struct _NMWifiPerformanceClass NMWifiPerformanceClass;

GType nm_wifi_performance_get_type(void);

NMWifiPerformance *nm_wifi_performance_get(void);

/* Returns NM_WIFI_PERFORMANCE_MODE_NONE for an unknown mode. */
NMWifiPerformanceMode nm_wifi_performance_mode_from_string(const char *str);

/* The modes of all held leases. */
NMWifiPerformanceMode nm_wifi_performance_get_modes(NMWifiPerformance *self);

/* Returns FALSE if @sender already holds a lease in another mode, or if
 * the lease could not be saved. */
gboolean nm_wifi_performance_acquire(NMWifiPerformance    *self,
                                     GDBusConnection      *connection,
                                     const char           *sender,
                                     NMWifiPerformanceMode mode,
                                     const char           *reason,
                                     GError              **error);
/* Returns TRUE if @sender holds no lease. Returns FALSE, keeping the lease,
 * if the release could not be saved. */
gboolean nm_wifi_performance_release(NMWifiPerformance *self, const char *sender, GError **error);

/* Picks up the leases saved by an NM that was restarted. */
void nm_wifi_performance_restore(NMWifiPerformance *self);

/* Return the list of held leases. */
GVariant *nm_wifi_performance_get_leases(NMWifiPerformance *self);

#endif /* __NM_WIFI_PERFORMANCE_H__ */
