#include <errno.h> /* errno */
#include <fcntl.h> /* open() */
/* kevent() */
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/ioctl.h> /* ioctl() */
/* APM macros */
#include <machine/apmvar.h>

#include "up-backend.h"
#include "up-daemon.h"
#include "up-marshal.h"
#include "up-device.h"

#define UP_BACKEND_SUSPEND_COMMAND	"/usr/sbin/zzz"

static void	up_backend_class_init	(UpBackendClass	*klass);
static void	up_backend_init	(UpBackend		*backend);
static void	up_backend_finalize	(GObject		*object);

#define UP_BACKEND_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), UP_TYPE_BACKEND, UpBackendPrivate))

struct UpBackendPrivate
{
	UpDaemon		*daemon;
	UpDevice		*device;
	UpDeviceList		*device_list; /* unused */
	GObject			*native;
	GThread			*apm_thread;
	int			apm_fd;
};

enum {
	SIGNAL_DEVICE_ADDED,
	SIGNAL_DEVICE_REMOVED,
	SIGNAL_LAST
};

static guint signals [SIGNAL_LAST] = { 0 };

G_DEFINE_TYPE (UpBackend, up_backend, G_TYPE_OBJECT)

/**
 * up_backend_add_cb:
 **/
static gboolean
up_backend_add_cb (UpBackend *backend)
{
	/* coldplug */
	if (!up_device_coldplug (backend->priv->device, backend->priv->daemon, backend->priv->native))
		g_warning ("failed to coldplug");
	/* emit */
	else
		g_signal_emit (backend, signals[SIGNAL_DEVICE_ADDED], 0, backend->priv->native, backend->priv->device);
	return FALSE;
}

/**
 * functions called by upower daemon
 **/

/**
 * up_backend_coldplug:
 * @backend: The %UpBackend class instance
 * @daemon: The %UpDaemon controlling instance
 *
 * Finds all the devices already plugged in, and emits device-add signals for
 * each of them.
 *
 * Return value: %TRUE for success
 **/
gboolean
up_backend_coldplug (UpBackend *backend, UpDaemon *daemon)
{
	backend->priv->daemon = g_object_ref (daemon);
	backend->priv->device_list = up_daemon_get_device_list (daemon);
	/* small delay until first device is added */
	g_timeout_add_seconds (1, (GSourceFunc) up_backend_add_cb, backend);

	return TRUE;
}


/**
 * up_backend_get_powersave_command:
 **/
const gchar *
up_backend_get_powersave_command (UpBackend *backend, gboolean powersave)
{
	return NULL;
}

/**
 * up_backend_get_suspend_command:
 **/
const gchar *
up_backend_get_suspend_command (UpBackend *backend)
{
	return UP_BACKEND_SUSPEND_COMMAND;
}

/**
 * up_backend_get_hibernate_command:
 **/
const gchar *
up_backend_get_hibernate_command (UpBackend *backend)
{
	return NULL;
}

/**
 * up_backend_kernel_can_suspend:
 **/
gboolean
up_backend_kernel_can_suspend (UpBackend *backend)
{
	return FALSE;
}

/**
 * up_backend_kernel_can_hibernate:
 **/
gboolean
up_backend_kernel_can_hibernate (UpBackend *backend)
{
	return FALSE;
}

gboolean
up_backend_has_encrypted_swap (UpBackend *backend)
{
	return FALSE;
}

/* Return value: a percentage value */
gfloat
up_backend_get_used_swap (UpBackend *backend)
{
	return 0;
}

/**
 * OpenBSD specific code
 **/

struct apm_power_info
up_backend_apm_get_power_info(int fd) {
	struct apm_power_info bstate;
	bstate.battery_state = 255;
	bstate.ac_state = 255;
	bstate.battery_life = 0;
	bstate.minutes_left = -1;

	if (-1 == ioctl(fd, APM_IOC_GETPOWER, &bstate))
		g_warning("ioctl on fd %d failed : %s", fd, g_strerror(errno));
	return bstate;
}

UpDeviceState up_backend_apm_get_battery_state_value(u_char battery_state) {
	switch(battery_state) {
		case APM_BATT_HIGH:
			return UP_DEVICE_STATE_FULLY_CHARGED;
		case APM_BATT_LOW:
			return UP_DEVICE_STATE_DISCHARGING; // XXXX
		case APM_BATT_CRITICAL:
			return UP_DEVICE_STATE_EMPTY;
		case APM_BATT_CHARGING:
			return UP_DEVICE_STATE_CHARGING;
		case APM_BATTERY_ABSENT:
			return UP_DEVICE_STATE_EMPTY;
		case APM_BATT_UNKNOWN:
			return UP_DEVICE_STATE_UNKNOWN;
	}
}

/* callback updating the device */
static gboolean
up_backend_apm_powerchange_event_cb(gpointer object)
{
	UpBackend *backend;
	struct apm_power_info a;
	GTimeVal timeval;

	g_return_if_fail (UP_IS_BACKEND (object));
	backend = UP_BACKEND (object);
	a = up_backend_apm_get_power_info(backend->priv->apm_fd);

	g_message("Got event, in callback, percentage=%d", a.battery_life);

	g_get_current_time (&timeval);
	g_object_set (backend->priv->device,
			"state", up_backend_apm_get_battery_state_value(a.battery_state),
			"percentage", a.battery_life,
			"update-time", (guint64) timeval.tv_sec,
			NULL);
	/* return false to not endless loop */
	return FALSE;
}

/* thread doing kqueue() on apm device */
static gpointer
up_backend_apm_event_thread(gpointer object)
{
	int kq, nevents;
	struct kevent ev;
	struct timespec ts = {600, 0}, sts = {0, 0};

	UpBackend *backend;

	g_return_if_fail (UP_IS_BACKEND (object));
	backend = UP_BACKEND (object);

	g_message("setting up apm thread");

	/* open /dev/apm */
	if ((backend->priv->apm_fd = open("/dev/apm", O_RDONLY)) == -1) {
		if (errno != ENXIO && errno != ENOENT)
			g_error("cannot open device file");
	}
	g_message("apm fd=%d", backend->priv->apm_fd);
	kq = kqueue();
	if (kq <= 0)
		g_error("kqueue");
	EV_SET(&ev, backend->priv->apm_fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR,
	    0, 0, NULL);
	nevents = 1;
	if (kevent(kq, &ev, nevents, NULL, 0, &sts) < 0)
		g_error("kevent");

	/* blocking wait on kqueue */
	for (;;) {
		int rv;

		/* 10mn timeout */
		sts = ts;
		if ((rv = kevent(kq, NULL, 0, &ev, 1, &sts)) < 0)
			break;
		if (!rv)
			continue;
		if (ev.ident == backend->priv->apm_fd && APM_EVENT_TYPE(ev.data) == APM_POWER_CHANGE ) {
			/* g_idle_add the callback */
			g_idle_add((GSourceFunc) up_backend_apm_powerchange_event_cb, backend);
		}
	}
	/* shouldnt be reached ? */
}

/**
 * GObject class functions
 **/

/**
 * up_backend_new:
 *
 * Return value: a new %UpBackend object.
 **/
UpBackend *
up_backend_new (void)
{
	UpBackend *backend;
	backend = g_object_new (UP_TYPE_BACKEND, NULL);
	return UP_BACKEND (backend);
}

/**
 * up_backend_class_init:
 * @klass: The UpBackendClass
 **/
static void
up_backend_class_init (UpBackendClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = up_backend_finalize;

	signals [SIGNAL_DEVICE_ADDED] =
		g_signal_new ("device-added",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (UpBackendClass, device_added),
			      NULL, NULL, up_marshal_VOID__POINTER_POINTER,
			      G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_POINTER);
	signals [SIGNAL_DEVICE_REMOVED] =
		g_signal_new ("device-removed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (UpBackendClass, device_removed),
			      NULL, NULL, up_marshal_VOID__POINTER_POINTER,
			      G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_POINTER);

	g_type_class_add_private (klass, sizeof (UpBackendPrivate));
}

/**
 * up_backend_init:
 **/
static void
up_backend_init (UpBackend *backend)
{
	GError *err = NULL;

	backend->priv = UP_BACKEND_GET_PRIVATE (backend);
	backend->priv->daemon = NULL;
	backend->priv->device_list = NULL;
	backend->priv->native = g_object_new (UP_TYPE_DEVICE, NULL);
	backend->priv->device = up_device_new ();

	g_thread_init (NULL);
	/* creates thread */
	if((backend->priv->apm_thread = g_thread_create((GThreadFunc)up_backend_apm_event_thread, backend, FALSE, &err) == NULL))
	{
		g_warning("Thread create failed: %s", err->message);
		g_error_free (err);
	}

	/* setup dummy */
	g_object_set (backend->priv->device,
		      "native-path", "/hal/blows/goats",
		      "vendor", "hughsie",
		      "model", "BAT1",
		      "serial", "0001",
		      "type", UP_DEVICE_KIND_BATTERY,
		      "online", FALSE,
		      "power-supply", TRUE,
		      "is-present", TRUE,
		      "is-rechargeable", TRUE,
		      "has-history", FALSE,
		      "has-statistics", FALSE,
		      "state", UP_DEVICE_STATE_DISCHARGING,
		      "energy", 0.0f,
		      "energy-empty", 0.0f,
		      "energy-full", 10.0f,
		      "energy-full-design", 10.0f,
		      "energy-rate", 5.0f,
		      "percentage", 50.0f,
		      NULL);
}

/**
 * up_backend_finalize:
 **/
static void
up_backend_finalize (GObject *object)
{
	UpBackend *backend;

	g_return_if_fail (UP_IS_BACKEND (object));

	backend = UP_BACKEND (object);

	if (backend->priv->daemon != NULL)
		g_object_unref (backend->priv->daemon);
	if (backend->priv->device_list != NULL)
		g_object_unref (backend->priv->device_list);
	g_object_unref (backend->priv->native);
	g_object_unref (backend->priv->device);
	/* XXX stop apm_thread ? */

	G_OBJECT_CLASS (up_backend_parent_class)->finalize (object);
}

