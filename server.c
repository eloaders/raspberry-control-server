/* Raspberry Control - Control Raspberry Pi with your Android Device
 *
 * Copyright (C) Lukasz Skalski <lukasz.skalski@op.pl>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#define _GNU_SOURCE
#include <gio/gio.h>
#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <ctype.h>
#include <dirent.h>
#include <pwd.h>
#include <jansson.h>
#include <libwebsockets.h>

#ifdef HAVE_SYSTEMD
#include <systemd/sd-journal.h>
#endif

#define MAX_PAYLOAD 10000


/*
 * Global variables
 */
static struct libwebsocket_context *context;
char board_revision[4];
char *notification;

gboolean opt_use_ssl = FALSE;
gboolean opt_no_daemon = FALSE;
gboolean opt_show_json_obj = FALSE;
gboolean exit_loop = FALSE;
gboolean send_notification = FALSE; 
gint port = 8080;


/*
 * Supported 1-wire temperature sensors
 */
#define DS18B20_CODE	"28"
#define DS1820_CODE	"10"


/*
 * Commandline options
 */
GOptionEntry entries[] =
{
  { "use-ssl", 's', 0, G_OPTION_ARG_NONE, &opt_use_ssl, "Use SSL to encrypt the connection between client and server", NULL},
  { "no-daemon", 'n', 0, G_OPTION_ARG_NONE, &opt_no_daemon, "Don't detach Raspberry Control into the background", NULL},
  { "show-json", 'j', 0, G_OPTION_ARG_NONE, &opt_show_json_obj, "Show JSON objects in daemon log file", NULL},
  { "port", 'p', 0, G_OPTION_ARG_INT, &port, "Port number [default: 8080]", NULL },
  { NULL }
};


/*
 * Static buffer
 */
struct per_session_data {
  unsigned char buf[LWS_SEND_BUFFER_PRE_PADDING + MAX_PAYLOAD + LWS_SEND_BUFFER_POST_PADDING];
  unsigned int len;
  unsigned int index;
};


/*
 *  print_log()
 */
static void
print_log (gint msg_priority, const gchar *msg, ...)
{
  va_list arg;
  va_start(arg, msg);

  GString *log = g_string_new(NULL);
  g_string_vprintf (log, msg, arg);

#ifdef DEBUG
  g_print ("%s", log->str);
#endif

#ifdef HAVE_SYSTEMD
  sd_journal_print (msg_priority, log->str);
#else
  syslog (msg_priority, log->str);
#endif

  g_string_free(log, TRUE);
  va_end(arg);
}


/*
 * SIGINT handler
 */
static gboolean
sigint_handler ()
{
  libwebsocket_cancel_service (context);
  exit_loop = TRUE;
  return TRUE;
}


/*
 * check_board_revision()
 */
gboolean check_board_revision ()
{
  FILE *fd;
  char fileline [200];
  char *ptr;

  fd = fopen("/proc/cpuinfo", "r");
  if (!fd)
    return FALSE;

  while (1)
    {
      if (fgets (fileline, 200, fd) == NULL)
        break;

      if (strncmp(fileline, "Revision", 8) == 0)
        {
          fileline [strlen(fileline)-1] = 0;
          ptr = strstr (fileline, ":");
	  if (ptr == NULL)
            {
              fclose (fd);
              return FALSE;
            }

          ptr += 2;
          strncpy (board_revision, ptr, 4);
          print_log (LOG_INFO, "(main) Board Revision: %s\n", board_revision);
          break;
        }
    }

  fclose (fd);
  return TRUE;
}


/*
 * dbus_notification_callback()
 */
void dbus_notification_callback (GDBusConnection  *connection,
                                 const gchar      *sender_name,
                                 const gchar      *object_path,
                                 const gchar      *interface_name,
                                 const gchar      *signal_name,
                                 GVariant         *parameters,
                                 gpointer          user_data)
{
  json_t *notification_obj;
  char *notification_msg;

  print_log (LOG_INFO, "(notification) NOTIFICATION\n");

  send_notification = FALSE;
  notification_msg = NULL;
  free (notification);

  /* UDisk - 'DeviceAdded' */
  if ((strcmp(interface_name, "org.freedesktop.UDisks") == 0) &&
      (strcmp(signal_name, "DeviceAdded") == 0))
      asprintf (&notification_msg, "[UDisks] %s", signal_name);

  /* CUPS - 'JobQueuedLocal' */
  if (strcmp(signal_name, "JobQueuedLocal") == 0)
    asprintf (&notification_msg, "[CUPS] %s", signal_name);

  if (!notification_msg)
    asprintf (&notification_msg, "(not set)");

  notification_obj = json_pack ("{s:s}", "Notification", notification_msg);
  notification = json_dumps (notification_obj, 0);

  free (notification_msg);
  json_decref (notification_obj);
  send_notification = TRUE;
}


/*
 * dbus_set_notification()
 */
void
dbus_set_notification (GDBusConnection  *connection,
                       const gchar      *sender,
                       const gchar      *interface_name,
                       const gchar      *member,
                       const gchar      *object_path)
{
  print_log (LOG_INFO, "(notification) new notification subscription\n");
  g_dbus_connection_signal_subscribe (connection,
                                      sender,
                                      interface_name,
                                      member,
                                      object_path,
                                      NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      dbus_notification_callback,
                                      NULL,
                                      NULL);
  /* TODO - collect all subscription identifiers */
}


/*
 * send_error()
 *
 * JSON Object
 * ===========
 *
 * {
 *   "Error" : "Can't open device"
 * }
 */
unsigned int
send_error (unsigned char *buffer, const char *error)
{
  json_t *error_obj;
  char *error_str;
  int error_len;

  error_obj = json_pack ("{s:s}", "Error", error);
  error_str = json_dumps (error_obj, 0);

  error_len = strlen (error_str);
  memcpy (buffer, error_str, error_len);

  json_decref (error_obj);
  free (error_str);
  return error_len;
}


/*
 * cmd_GetGPIO()
 *
 * JSON Object
 * ===========
 *
 * {
 *   "GPIOState": [
 *     {
 *       "gpio"     : 6,
 *       "value"    : 1,
 *       "direction": "in"
 *     },
 *     {
 *       "gpio"     : 3,
 *       "value"    : 0,
 *       "direction": "out"
 *     },
 *     .
 *     .
 *   ],
 *   "Revision" : "0002"
 * }
 */
unsigned int
cmd_GetGPIO (struct libwebsocket *wsi, unsigned char *buffer)
{
  json_t *gpio_obj;
  json_t *gpio_array_obj;

  DIR *gpio_dir;
  FILE *fd, *fp;
  struct dirent *gpio_num_dir;

  char filepath [PATH_MAX];
  char fileline1 [5];
  char fileline2 [5];

  char *gpio_str;
  int gpio_len;

  print_log (LOG_INFO, "(%p) (cmd_GetGPIO) processing request\n", wsi);

  gpio_dir = opendir ("/sys/class/gpio");
  if (gpio_dir == NULL)
    {
      print_log (LOG_ERR, "(%p) (cmd_GetGPIO) unable to read the list of exported GPIO's\n", wsi);
      return send_error (buffer, "Unable to read the list of exported GPIO's");
    }

  gpio_obj = json_object();
  gpio_array_obj = json_array();

  while(1)
    {
      json_t *gpio_num_obj;

      gpio_num_dir = readdir (gpio_dir);
      if (gpio_num_dir == NULL)
        break;

      if ((gpio_num_dir->d_type != DT_LNK))
        continue;

      if (strncmp (gpio_num_dir->d_name, "gpio", 4) != 0)
        continue;

      /* read 'value' */
      snprintf (filepath, PATH_MAX, "/sys/class/gpio/%s/value", gpio_num_dir->d_name);
      fd = fopen (filepath, "r");
      if (fd == NULL)
        continue;

      /* read 'direction' */
      snprintf (filepath, PATH_MAX, "/sys/class/gpio/%s/direction", gpio_num_dir->d_name);
      fp = fopen (filepath, "r");
      if (fp == NULL)
        {
          fclose (fd);
          continue;
        }

      fgets (fileline1, 5, fd);
      fgets (fileline2, 5, fp);
      fileline2 [strlen(fileline2)-1] = 0;

      gpio_num_obj = json_pack ("{s:i, s:i, s:s}",
                                "gpio", atoi ((gpio_num_dir->d_name) + 4),
                                "value", atoi (fileline1),
                                "direction", fileline2);

      json_array_append (gpio_array_obj, gpio_num_obj);
      json_decref (gpio_num_obj);

      fclose (fd);
      fclose (fp);
    }

  json_object_set_new (gpio_obj, "Revision", json_string (board_revision));
  json_object_set (gpio_obj, "GPIOState", gpio_array_obj);
  if (gpio_obj == NULL)
    {
      print_log (LOG_ERR, "(%p) (cmd_GetGPIO) can't prepare valid JSON object\n", wsi);
      json_decref (gpio_array_obj);
      json_decref (gpio_obj);
      return send_error (buffer, "Can't prepare valid JSON object");
    }

  gpio_str = json_dumps (gpio_obj, 0);
  if (gpio_str == NULL)
    {
      print_log (LOG_ERR, "(%p) (cmd_GetGPIO) can't prepare valid JSON object\n", wsi);
      json_decref (gpio_array_obj);
      json_decref (gpio_obj);
      return send_error (buffer, "Can't prepare valid JSON object");
    }

  gpio_len = strlen (gpio_str);
  memcpy (buffer, gpio_str, gpio_len);

  if (opt_show_json_obj)
    print_log (LOG_INFO, "(%p) (cmd_GetGPIO) %s\n", wsi, gpio_str);

  json_decref (gpio_array_obj);
  json_decref (gpio_obj);
  free (gpio_str);

  return gpio_len;
}


/*
 * cmd_GetTempSensors()
 *
 * JSON Object
 * ===========
 *
 * {
 *   "TempSensors": [
 *     {
 *       "type"  : "DS18B20",
 *       "id"    : "28-000002f218f8",
 *       "temp"  : 23.250,
 *       "crc"   : "YES"
 *     },
 *     {
 *       "type" : "DS18S20",
 *       "id"   : "10-000002f1f367",
 *       "temp" : 23.562,
 *       "crc"  : "NO"
 *     },
 *     .
 *     .
 *   ]
 * }
 */
unsigned int
cmd_GetTempSensors (struct libwebsocket *wsi, unsigned char *buffer)
{
  json_t *tempsensors_obj;
  json_t *tempsensors_array_obj;

  FILE *fd, *fp;
  DIR *w1_master_dir;
  struct dirent *w1_device_dir;

  char filepath [PATH_MAX];
  char fileline [100];
  int num_line;

  char *tempsensors_str;
  int tempsensors_len;

  print_log (LOG_INFO, "(%p) (cmd_GetTempSensors) processing request\n", wsi);

  /* don't scan 1-wire bus if daemon has limited privileges */
  if (geteuid() == 0)
    {
      /* remove all sensors */
      fd = fopen("/sys/bus/w1/devices/w1_bus_master1/w1_master_slaves", "r");
      if (!fd)
        {
          print_log (LOG_ERR, "(%p) (cmd_GetTempSensors) unable to read the list of registered 1-wire sensors\n", wsi);
          return send_error (buffer, "Unable to read the list of registered 1-wire sensors");
        }
      while (1)
        {
          if (fgets (fileline, 20, fd) == NULL)
            break;

          fp = fopen("/sys/bus/w1/devices/w1_bus_master1/w1_master_remove", "w");
          if (!fp)
            {
              print_log (LOG_ERR, "(%p) (cmd_GetTempSensors) unable to remove previously registered 1-wire sensor\n", wsi);
              fclose (fd);
              return send_error (buffer, "Unable to remove previously registered 1-wire sensor");
            }

          fprintf (fp, "%s", fileline);
          fclose (fp);
        }
      fclose (fd);

      /* rescan 1-wire bus */
      fd = fopen("/sys/bus/w1/devices/w1_bus_master1/w1_master_search", "w");
      if (!fd)
        {
          print_log (LOG_ERR, "(%p) (cmd_GetTempSensors) unable to rescan 1-wire sensors\n", wsi);
          return send_error (buffer, "Unable to rescan 1-wire sensors");
        }
      fprintf (fd, "1");
      fclose (fd);

      /* we have to wait till all sensors will be available on bus */
      sleep (1);
    }

  /* read all sensors */
  w1_master_dir = opendir ("/sys/devices/w1_bus_master1");
  if (w1_master_dir == NULL)
    {
      print_log (LOG_ERR, "(%p) (cmd_GetTempSensors) can't open 'w1_bus_master1' directory\n", wsi);
      return send_error (buffer, "Can't open 'w1_bus_master1' directory");
    }

  tempsensors_obj = json_object();
  tempsensors_array_obj = json_array();

  while(1)
    {
      json_t *tempsensor_obj;

      w1_device_dir = readdir (w1_master_dir);
      if (w1_device_dir == NULL)
        break;

      if ((w1_device_dir->d_type != DT_DIR))
        continue;

      if ((strncmp (w1_device_dir->d_name, DS18B20_CODE, 2) != 0) &&
          (strncmp (w1_device_dir->d_name, DS1820_CODE, 2) != 0))
        continue;

      snprintf (filepath, PATH_MAX, "/sys/devices/w1_bus_master1/%s/w1_slave", w1_device_dir->d_name);
      fd = fopen (filepath, "r");
      if (fd == NULL)
        continue;

      num_line = 1;
      tempsensor_obj = json_object();

      while (1)
        {
          if (fgets (fileline, 100, fd) == NULL)
            break;

          if (strncmp (w1_device_dir->d_name, DS18B20_CODE, 2) == 0)
            json_object_set_new (tempsensor_obj, "type", json_string ("Dallas DS18B20"));

          if (strncmp (w1_device_dir->d_name, DS1820_CODE, 2) == 0)
            json_object_set_new (tempsensor_obj, "type", json_string ("Dallas DS1820"));

          if (num_line == 1)
            {
              json_object_set_new (tempsensor_obj, "id", json_string (w1_device_dir->d_name));
              fileline [strlen(fileline)-1] = 0;
              json_object_set_new (tempsensor_obj, "crc", json_string (&fileline[36]));
            }
          else if (num_line == 2)
            {
              json_object_set_new (tempsensor_obj, "temp", json_real ((atof (&fileline[29])) / 1000));
            }
          num_line++;
        }

      json_array_append (tempsensors_array_obj, tempsensor_obj);
      json_decref (tempsensor_obj);
      
      fclose (fd);
    }

  json_object_set (tempsensors_obj, "TempSensors", tempsensors_array_obj);
  if (tempsensors_obj == NULL)
    {
      print_log (LOG_ERR, "(%p) (cmd_GetTempSensors) can't prepare valid JSON object\n", wsi);
      json_decref (tempsensors_array_obj);
      json_decref (tempsensors_obj);
      return send_error (buffer, "Can't prepare valid JSON object");
    }

  tempsensors_str = json_dumps (tempsensors_obj, 0);
  if (tempsensors_str == NULL)
    {
      print_log (LOG_ERR, "(%p) (cmd_GetTempSensors) can't prepare valid JSON object\n", wsi);
      json_decref (tempsensors_array_obj);
      json_decref (tempsensors_obj);
      return send_error (buffer, "Can't prepare valid JSON object");
    }

  tempsensors_len = strlen (tempsensors_str);
  memcpy (buffer, tempsensors_str, tempsensors_len);

  if (opt_show_json_obj)
    print_log (LOG_INFO, "(%p) (cmd_GetTempSensors) %s\n", wsi, tempsensors_str);

  json_decref (tempsensors_array_obj);
  json_decref (tempsensors_obj);
  free (tempsensors_str);

  return tempsensors_len;
}


/*
 * cmd_GetProcesses()
 *
 * JSON Object
 * ===========
 *
 * {
 *   "Processes": [
 *     {
 *       "pid"  : 1,
 *       "user" : "root",
 *       "name" : "init",
 *       "state": "S (sleeping)"
 *     },
 *     {
 *       "pid"  : 1934,
 *       "user" : "root",
 *       "name" : "rsyslogd",
 *       "state": "S (sleeping)"
 *     },
 *     .
 *     .
 *   ]
 * }
 */
unsigned int
cmd_GetProcesses (struct libwebsocket *wsi, unsigned char *buffer)
{
  json_t *proc_obj;
  json_t *proc_array_obj;

  FILE *proc_pid_status;
  DIR *proc_dir;
  struct dirent *proc_pid_dir;

  char filepath [PATH_MAX];
  char fileline [1000];
  char *ptr;

  char *proc_str;
  int proc_len;

  print_log (LOG_INFO, "(%p) (cmd_GetProcesses) processing request\n", wsi);

  proc_dir = opendir ("/proc");
  if (proc_dir == NULL)
    {
      print_log (LOG_ERR, "(%p) (cmd_GetProcesses) unable to read the list of processes\n", wsi);
      return send_error (buffer, "Unable to read the list of processes");
    }

  proc_obj = json_object();
  proc_array_obj = json_array();

  while(1)
    {
      json_t *proc_entry_obj;

      proc_pid_dir = readdir (proc_dir);
      if (proc_pid_dir == NULL)
        break;

      if ((proc_pid_dir->d_type != DT_DIR) || (!isdigit((unsigned char) proc_pid_dir->d_name[0])))
        continue;

      snprintf(filepath, PATH_MAX, "/proc/%s/status", proc_pid_dir->d_name);
      proc_pid_status = fopen(filepath, "r");
      if (proc_pid_status == NULL)
        continue;

      proc_entry_obj = json_object();
      json_object_set_new (proc_entry_obj, "pid", json_integer (atoi (proc_pid_dir->d_name)));

      while (1)
        {
          if (fgets (fileline, 1000, proc_pid_status) == NULL)
            break;

          if (strncmp(fileline, "Name:", 5) == 0)
            {
              fileline [strlen(fileline)-1] = 0;
              for (ptr = fileline + 5; *ptr != '\0' && isspace((unsigned char) *ptr);)
                ptr++;

              json_object_set_new (proc_entry_obj, "name", json_string (ptr));
            }

          if (strncmp(fileline, "Uid:", 4) == 0)
            {
              struct passwd *pw;

              if ((pw = getpwuid ((unsigned int) strtol(fileline + 4, NULL, 10))))
                json_object_set_new (proc_entry_obj, "user", json_string (pw->pw_name));
            }

          if (strncmp(fileline, "State:", 6) == 0)
            {
              fileline [strlen(fileline)-1] = 0;
              for (ptr = fileline + 6; *ptr != '\0' && isspace((unsigned char) *ptr);)
                ptr++;

              json_object_set_new (proc_entry_obj, "state", json_string (ptr));
            }
        }

      json_array_append (proc_array_obj, proc_entry_obj);
      json_decref (proc_entry_obj);

      fclose (proc_pid_status);
    }

  json_object_set (proc_obj, "Processes", proc_array_obj);
  if (proc_obj == NULL)
    {
      print_log (LOG_ERR, "(%p) (cmd_Processes) can't prepare valid JSON object\n", wsi);
      json_decref (proc_array_obj);
      json_decref (proc_obj);
      return send_error (buffer, "Can't prepare valid JSON object");
    }

  proc_str = json_dumps (proc_obj, 0);
  if (proc_str == NULL)
    {
      print_log (LOG_ERR, "(%p) (cmd_GetProcesses) can't prepare valid JSON object\n", wsi);
      json_decref (proc_array_obj);
      json_decref (proc_obj);
      return send_error (buffer, "Can't prepare valid JSON object");
    }

  proc_len = strlen (proc_str);
  memcpy (buffer, proc_str, proc_len);

  if (opt_show_json_obj)
    print_log (LOG_INFO, "(%p) (cmd_GetProcesses) %s\n", wsi, proc_str);

  json_decref (proc_array_obj);
  json_decref (proc_obj);
  free (proc_str);

  return proc_len;
}


/*
 * cmd_GetStatistics()
 *
 * JSON Object
 * ===========
 *
 * {
 *   "Statistics": {
 *      "kernel": "3.2.27+",
 *      "uptime": "1h 16m 39s",
 *      "serial": "00000000b62b4ab1",
 *      "mac_addr": "a1:eb:27:13:aa:b3",
 *      "used_space": 2.21,
 *      "free_space": 7.23,
 *      "ram_usage": 51,
 *      "swap_usage": 24,
 *      "cpu_load": "0.00 0.01 0.05",
 *      "cpu_temp": 44,
 *      "cpu_usage": 23
 *   }
 * }
 *
 */
unsigned int
cmd_GetStatistics (struct libwebsocket *wsi, unsigned char *buffer)
{
  json_t *stat_obj;
  char *stat_str;
  int stat_len;

  char *kernel, *uptime, *serial, *mac_addr, *cpu_load;
  int ram_usage, swap_usage, cpu_temp, cpu_usage;
  float used_space, free_space;

  print_log (LOG_INFO, "(%p) (cmd_GetStatistics) processing request\n", wsi);

  //Maciek, please add your code here!
  kernel = "3.2.27+";
  uptime = "1h 16m 39s";
  serial = "00000000b62b4ab1";
  mac_addr = "a1:eb:27:13:aa:b3";
  used_space = (float) (rand() % 101);
  free_space=  (float) (rand() % 101);
  ram_usage=  rand() % 101;
  swap_usage=  rand() % 101;
  cpu_load = "0.00 0.01 0.05";
  cpu_temp=  rand() % 101;
  cpu_usage=  rand() % 101;
  //

  stat_obj = json_pack ("{s:{s:s, s:s, s:s, s:s, s:f, s:f, s:i, s:i, s:s, s:i, s:i}}",
                        "Statistics",
                        "kernel", kernel,
                        "uptime", uptime,
                        "serial", serial,
                        "mac_addr", mac_addr,
                        "used_space", used_space,
                        "free_space", free_space,
                        "ram_usage", ram_usage,
                        "swap_usage", swap_usage,
                        "cpu_load", cpu_load,
                        "cpu_temp", cpu_temp,
                        "cpu_usage", cpu_usage);
  if (stat_obj == NULL)
    {
      print_log (LOG_ERR, "(%p) (cmd_GetStatistics) can't prepare valid JSON object\n", wsi);
      return send_error (buffer, "Can't prepare valid JSON object");
    }

  stat_str = json_dumps (stat_obj, 0);
  if (stat_str == NULL)
    {
      print_log (LOG_ERR, "(%p) (cmd_GetStatistics) can't prepare valid JSON object\n", wsi);
      json_decref (stat_obj);
      return send_error (buffer, "Can't prepare valid JSON object");
    }

  stat_len = strlen (stat_str);
  memcpy (buffer, stat_str, stat_len);

  if (opt_show_json_obj)
    print_log (LOG_INFO, "(%p) (cmd_GetStatistics) %s\n", wsi, stat_str);

  free (stat_str);
  json_decref (stat_obj);
  return stat_len;
}


/*
 * cmd_SendIR()
 */
unsigned int
cmd_SendIR (struct libwebsocket *wsi, unsigned char *buffer, char *args)
{
  char *cmd;
  int ret;

  print_log (LOG_INFO, "(%p) (cmd_SendIR) processing request\n", wsi);

  ret = asprintf (&cmd, "irsend SEND_ONCE %s", args);
  if (ret < 0)
    {
      print_log (LOG_ERR, "(%p) (cmd_SendIR) can't prepare LIRC command\n", wsi);
      return send_error (buffer, "Can't prepare LIRC command");
    }

  /*
   * Invoking system() function is temporary solution - we'll switch soon to
   * liblirc_client and lirc_send_one() - which will be available in next lirc
   * release.
   */
  ret = system (cmd);
  if (ret != 0)
    {
      print_log (LOG_ERR, "(%p) (cmd_SendIR) can't send signal\n", wsi);
      free (cmd);
      return send_error (buffer, "Can't send signal - please check server's log");
    }

  free (cmd);
  return 0;
}


/*
 * cmd_SetGPIO()
 */
unsigned int
cmd_SetGPIO (struct libwebsocket *wsi, unsigned char *buffer, char *args)
{
  FILE *fd;
  char filepath [PATH_MAX];
  char *gpio_num;
  char *gpio_act;

  print_log (LOG_INFO, "(%p) (cmd_SetGPIO) processing request\n", wsi);

  /* TODO - strcpy? */
  gpio_num = strtok (args, " ");
  gpio_act = strtok (NULL, " ");

  if ((strcmp(gpio_act, "1") == 0) || (strcmp(gpio_act, "0") == 0))
    {
      snprintf (filepath, PATH_MAX, "/sys/class/gpio/gpio%s/value", gpio_num);
      fd = fopen(filepath, "w");
      if (!fd)
        {
          print_log (LOG_ERR, "(%p) (cmd_SetGPIO) Unable to change GPIO value\n", wsi);
          return send_error (buffer, "Unable to change GPIO value");
        }
      fprintf (fd, "%s", gpio_act);
      fclose (fd);
    }
  else if ((strcmp(gpio_act, "in") == 0) || (strcmp(gpio_act, "out") == 0))
    {
      snprintf (filepath, PATH_MAX, "/sys/class/gpio/gpio%s/direction", gpio_num);
      fd = fopen(filepath, "w");
      if (!fd)
        {
          print_log (LOG_ERR, "(%p) (cmd_SetGPIO) Unable to change GPIO direction\n", wsi);
          return send_error (buffer, "Unable to change GPIO direction");
        }
      fprintf (fd, "%s", gpio_act);
      fclose (fd);
    }
  else
    {
      print_log (LOG_ERR, "(%p) (cmd_SetGPIO) Unsupported value - please report a bug\n", wsi);
      return send_error (buffer, "Unsupported value - please report a bug");
    }

  return cmd_GetGPIO (wsi, buffer);
}


/*
 * cmd_KillProcess()
 */
unsigned int
cmd_KillProcess (struct libwebsocket *wsi, unsigned char *buffer, char *pid_str)
{
  unsigned int pid;

  print_log (LOG_INFO, "(%p) (cmd_KillProcess) processing request\n", wsi);

  if (pid_str)
    {
      pid = atoi (pid_str);
      if (pid)
        {
          gint ret;

          ret = kill (pid, SIGKILL);
          if (ret < 0)
            goto error;
          print_log (LOG_INFO, "(%p) (cmd_KillProcess) send SIGKILL to PID %d\n", wsi, (int) pid);
        }
      else
        {
          goto error;
        }
    }
  else
    {
      goto error;
    }
  
  return cmd_GetProcesses (wsi, buffer);

error:
  print_log (LOG_ERR, "(%p) (cmd_KillProcess) Can't kill selected process\n", wsi);
  return send_error (buffer, "Can't kill selected process");
}


/*
 * parse_json()
 */
unsigned int
parse_json (struct libwebsocket  *wsi,
            unsigned char        *data,
            unsigned char        *buffer)
{
  json_t *root;
  json_error_t error;
  char *cmd_str, *args_str;
  int result;
  unsigned int len = 0;

  root = json_loads ((char*) data, 0, &error);
  if(!root)
    {
      print_log (LOG_ERR, "(%p) (cmd_parser) parser error on line %d: %s\n", wsi, error.line, error.text);
      return send_error (buffer, "Could not parse command");
    }

  result = json_unpack (root, "{s:{s:s, s:s}}", "RunCommand", "cmd", &cmd_str, "args", &args_str);
  if (result < 0)
    {
      print_log (LOG_ERR, "(%p) (cmd_parser) not valid JSON data\n", wsi);
      json_decref (root);
      return send_error (buffer, "Could not parse command - not valid JSON data");
    }

  if (strcmp(cmd_str, "GetGPIO") == 0) 
    len = cmd_GetGPIO (wsi, buffer);
  else if (strcmp(cmd_str, "GetTempSensors") == 0)
    len = cmd_GetTempSensors (wsi, buffer);
  else if (strcmp(cmd_str, "GetProcesses") == 0)
    len = cmd_GetProcesses (wsi, buffer);
  else if (strcmp(cmd_str, "GetStatistics") == 0)
    len = cmd_GetStatistics (wsi, buffer);
  else if (strcmp(cmd_str, "SendIR") == 0)
    len = cmd_SendIR (wsi, buffer, args_str);
  else if (strcmp(cmd_str, "SetGPIO") == 0)
    len = cmd_SetGPIO (wsi, buffer, args_str);
  else if (strcmp(cmd_str, "KillProcess") == 0)
    len = cmd_KillProcess (wsi, buffer, args_str);
  else 
    {
      print_log (LOG_ERR, "(%p) (cmd_parser) not supported command\n", wsi);
      json_decref (root);
      return send_error (buffer, "Not supported command");
    }

  /* TODO - free cmd_str and args_str? */
  json_decref (root);
  return len;
} 


/*
 * raspberry_control_callback()
 */
static int
raspberry_control_callback (struct libwebsocket_context *context,
	                    struct libwebsocket *wsi,
	                    enum libwebsocket_callback_reasons reason,
                            void *user, void *in, size_t len)
{
  struct per_session_data *psd = (struct per_session_data*) user;
  int nbytes;

  switch (reason)
    {

      case LWS_CALLBACK_ESTABLISHED: 
        print_log (LOG_INFO, "(%p) (callback) connection established\n", wsi);
      break;

      case LWS_CALLBACK_CLOSED:
        print_log (LOG_INFO, "(%p) (callback) connection closed\n", wsi);
      break;

      case LWS_CALLBACK_SERVER_WRITEABLE:

        /* broadcast message */
        if (psd->buf[LWS_SEND_BUFFER_PRE_PADDING] == 0)
          {
            psd->len = strlen (notification);
            if (psd->len == 0)
              return 0;
            memcpy (&psd->buf[LWS_SEND_BUFFER_PRE_PADDING], notification, psd->len);
          }

        nbytes = libwebsocket_write(wsi, &psd->buf[LWS_SEND_BUFFER_PRE_PADDING], psd->len, LWS_WRITE_TEXT);
        memset (&psd->buf[LWS_SEND_BUFFER_PRE_PADDING], 0, psd->len);
        print_log (LOG_INFO, "(%p) (callback) %d bytes written\n", wsi, nbytes);
        if (nbytes < 0)
          {
            print_log (LOG_ERR, "(%p) (callback) %d bytes writing to socket, hanging up\n", wsi, nbytes);
            return 1;
          }
        if (nbytes < (int)psd->len)
          {
            print_log (LOG_ERR, "(%p) (callback) partial write\n", wsi);
            return -1; /*TODO*/
          }
      break;

      case LWS_CALLBACK_RECEIVE:
        print_log (LOG_INFO, "(%p) (callback) received %d bytes\n", wsi, (int) len);
        if (len > MAX_PAYLOAD)
          {
            print_log (LOG_ERR, "(%p) (callback) packet bigger than %u, hanging up\n", wsi, MAX_PAYLOAD);
            return 1;
          }

        psd->len = parse_json (wsi, in, &psd->buf[LWS_SEND_BUFFER_PRE_PADDING]);
        if (psd->len > 0)
          {
            libwebsocket_callback_on_writable (context, wsi);
          }
      break;

      default:
      break;
    }

  return 0;
}


/*
 * Defined protocols
 */
static struct libwebsocket_protocols protocols[] = {
  {
    "raspberry_control_protocol",     /* protocol name */
    raspberry_control_callback,       /* callback */
    sizeof(struct per_session_data)   /* max frame size / rx buffer */
  },
  {
    NULL, NULL, 0
  }
};


/*
 * main function
 */
int
main(int argc, char **argv)
{
  GDBusConnection *connection = NULL;
  GOptionContext *option_context = NULL;
  GError *error = NULL;

  char cert_path [1024];
  char key_path [1024];
  char *res_path = "/path/to/cert";

  gint cnt = 0;
  gint signal_id = 0;
  gint exit_value = EXIT_SUCCESS;
  struct lws_context_creation_info info;

  /* parse commandline options */
  option_context = g_option_context_new ("- Raspberry Control Daemon");
  g_option_context_add_main_entries (option_context, entries, NULL);
  if (!g_option_context_parse (option_context, &argc, &argv, &error))
    {
      g_printerr ("%s: %s\n", argv[0], error->message);
      exit_value = EXIT_FAILURE;
      goto out;
    }

  /* deamonize */
  if (!opt_no_daemon && lws_daemonize("/var/run/lock/.raspberry-control-lock"))
    {
      g_printerr ("%s: failed to daemonize\n", argv[0]);
      exit_value = EXIT_FAILURE;
      goto out;
    }

  /* open syslog */
#ifndef HAVE_SYSTEMD
  openlog("Raspberry Control Daemon", LOG_NOWAIT|LOG_PID, LOG_USER);
#endif

  /* fill 'lws_context_creation_info' struct */ 
  memset (&info, 0, sizeof info);
  info.port = port;
  info.iface = NULL;
  info.protocols = protocols;
  info.extensions = libwebsocket_get_internal_extensions();
  info.gid = -1;
  info.uid = -1;
  info.options = 0;

  /* set cert and private key filepaths */
  if (!opt_use_ssl)
    {
      info.ssl_cert_filepath = NULL;
      info.ssl_private_key_filepath = NULL;
    } 
  else
    {
      if (strlen (res_path) > sizeof(cert_path) - 32)
        {
          print_log (LOG_ERR, "(main) resource path too long\n");
          exit_value = EXIT_FAILURE;
          goto out;
        }

      sprintf (cert_path, "%s/raspberry-control-daemon.pem", res_path);

      if (strlen (res_path) > sizeof(key_path) - 32)
        {
          print_log (LOG_ERR, "(main) resource path too long\n");
          exit_value = EXIT_FAILURE;
          goto out;
        }

      sprintf (key_path, "%s/raspberry-control-daemon.key.pem", res_path);

      info.ssl_cert_filepath = cert_path;
      info.ssl_private_key_filepath = key_path;
    }

  /* check board revision */
  if (!check_board_revision())
    print_log (LOG_ERR, "(main) Something goes wrong - can't check board revision\n");

  /* connect to the bus */
  connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
  if (connection == NULL)
    {
      print_log (LOG_ERR, "(main) Error connecting to D-Bus: %s - some notification won't be available\n", error->message);
      g_error_free (error);
    }
  else
    {
      print_log (LOG_INFO, "(main) Connected to D-Bus\n");

      /* UDisks - 'DeviceAdded' */
      dbus_set_notification (connection, "org.freedesktop.UDisks", NULL, "DeviceAdded", NULL);

      /* CUPS - 'JobQueuedLocal' */
      dbus_set_notification (connection, NULL, "com.redhat.PrinterSpooler", "JobQueuedLocal", NULL);
    }

  /* handle SIGINT */
  signal_id = g_unix_signal_add (SIGINT, sigint_handler, NULL);

  /* create context */
  context = libwebsocket_create_context (&info);
  if (context == NULL)
    {
      print_log (LOG_ERR, "(main) libwebsocket context init failed\n");
      goto out;
    }
  print_log (LOG_INFO, "(main) context - %p\n", context);

  /* main loop */
  while (cnt >= 0 && !exit_loop)
    {
      cnt = libwebsocket_service (context, 10);

      if (send_notification)
        {
          libwebsocket_callback_on_writable_all_protocol (&protocols[0]);
          send_notification = FALSE;
        }

      g_main_context_iteration (NULL, FALSE);
    }

out:

  if (context != NULL)
    libwebsocket_context_destroy (context);
  if (signal_id > 0)
    g_source_remove (signal_id);
  if (option_context != NULL)
    g_option_context_free (option_context);

#ifndef HAVE_SYSTEMD
  closelog();
#endif

  return exit_value;
}
