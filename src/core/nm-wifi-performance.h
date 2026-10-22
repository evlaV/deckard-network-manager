/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef __NM_WIFI_PERFORMANCE_H__
#define __NM_WIFI_PERFORMANCE_H__

typedef enum {
    NM_WIFI_PERFORMANCE_MODE_NONE = 0,

    /* Disables Wi-Fi powersave on client connections, overriding
     * wifi.powersave. */
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

#define NM_WIFI_PERFORMANCE_LEASES_CHANGED "leases-changed"

typedef struct _NMWifiPerformance      NMWifiPerformance;
typedef struct _NMWifiPerformanceClass NMWifiPerformanceClass;

GType nm_wifi_performance_get_type(void);

NMWifiPerformance *nm_wifi_performance_get(void);

NMWifiPerformanceMode nm_wifi_performance_mode_from_string(const char *str);

NMWifiPerformanceMode nm_wifi_performance_get_modes(NMWifiPerformance *self);

gboolean nm_wifi_performance_acquire(NMWifiPerformance    *self,
                                     GDBusConnection      *connection,
                                     const char           *sender,
                                     NMWifiPerformanceMode mode,
                                     const char           *reason,
                                     GError              **error);
gboolean nm_wifi_performance_release(NMWifiPerformance *self, const char *sender, GError **error);

void nm_wifi_performance_restore(NMWifiPerformance *self);

#endif /* __NM_WIFI_PERFORMANCE_H__ */
