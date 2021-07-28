/*
  Copyright_License {

  XCSoar Glide Computer - http://www.xcsoar.org/
  Copyright (C) 2000-2021 The XCSoar Project
  A detailed list of copyright holders can be found in the file "AUTHORS".

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
}
*/

#include "PortDataField.hpp"
#include "Device/Features.hpp"
#include "Form/DataField/Enum.hpp"
#include "Language/Language.hpp"
#include "util/StringCompare.hxx"

#ifdef HAVE_POSIX
#include "Device/Port/TTYEnumerator.hpp"
#endif

#ifdef ANDROID
#include "java/Global.hxx"
#include "java/String.hxx"
#include "Android/Main.hpp"
#include "Android/BluetoothHelper.hpp"
#include "Android/UsbSerialHelper.hpp"
#include "Device/Port/AndroidIOIOUartPort.hpp"
#endif

static constexpr struct {
  DeviceConfig::PortType type;
  const TCHAR *label;
} port_types[] = {
  { DeviceConfig::PortType::DISABLED, N_("Disabled") },
#ifdef HAVE_INTERNAL_GPS
  { DeviceConfig::PortType::INTERNAL, N_("Built-in GPS & sensors") },
#endif
#ifdef ANDROID
  { DeviceConfig::PortType::RFCOMM_SERVER, N_("Bluetooth server") },
  { DeviceConfig::PortType::DROIDSOAR_V2, _T("DroidSoar V2") },
  { DeviceConfig::PortType::GLIDER_LINK, _T("GliderLink traffic receiver") },
#ifndef NDEBUG
  { DeviceConfig::PortType::NUNCHUCK, N_("IOIO switches and Nunchuk") },
#endif
  { DeviceConfig::PortType::I2CPRESSURESENSOR, N_("IOIO I²C pressure sensor") },
  { DeviceConfig::PortType::IOIOVOLTAGE, N_("IOIO voltage sensor") },
#endif

  { DeviceConfig::PortType::TCP_CLIENT, N_("TCP client") },

  /* label not translated for now, until we have a TCP/UDP port
     selection UI */
  { DeviceConfig::PortType::TCP_LISTENER, N_("TCP port") },
  { DeviceConfig::PortType::UDP_LISTENER, N_("UDP port") },

  { DeviceConfig::PortType::SERIAL, nullptr } /* sentinel */
};

/** the number of fixed port types (excludes Serial, Bluetooth and IOIOUart) */
static constexpr unsigned num_port_types = std::size(port_types) - 1;

static unsigned
AddPort(DataFieldEnum &df, DeviceConfig::PortType type,
        const TCHAR *text, const TCHAR *display_string=nullptr,
        const TCHAR *help=nullptr) noexcept
{
  /* the upper 16 bit is the port type, and the lower 16 bit is a
     serial number to make the enum id unique */

  unsigned id = ((unsigned)type << 16) + df.Count();
  df.AddChoice(id, text, display_string, help);
  return id;
}

#if defined(HAVE_POSIX)

static bool
DetectSerialPorts(DataFieldEnum &df) noexcept
{
  TTYEnumerator enumerator;
  if (enumerator.HasFailed())
    return false;

  unsigned sort_start = df.Count();

  bool found = false;
  const char *path;
  while ((path = enumerator.Next()) != nullptr) {
    const char *display_string = StringAfterPrefix(path, "/dev/");
    if (display_string == nullptr)
      display_string = path;

    AddPort(df, DeviceConfig::PortType::SERIAL, path, display_string);
    found = true;
  }

  if (found)
    df.Sort(sort_start);

  return found;
}

#endif

#if defined(_WIN32) && !defined(HAVE_POSIX)

static void
FillDefaultSerialPorts(DataFieldEnum &df) noexcept
{
  for (unsigned i = 1; i <= 10; ++i) {
    TCHAR buffer[64];
    _stprintf(buffer, _T("COM%u:"), i);
    AddPort(df, DeviceConfig::PortType::SERIAL, buffer);
  }
}

#endif

static void
FillPortTypes(DataFieldEnum &df, const DeviceConfig &config) noexcept
{
  for (unsigned i = 0; port_types[i].label != nullptr; i++) {
    unsigned id = AddPort(df, port_types[i].type, port_types[i].label,
                          gettext(port_types[i].label));

    if (port_types[i].type == config.port_type)
      df.Set(id);
  }
}

void
SetPort(DataFieldEnum &df, DeviceConfig::PortType type,
        const TCHAR *value) noexcept
{
  assert(value != nullptr);

  if (!df.Set(value))
    df.Set(AddPort(df, type, value));
}

static void
FillSerialPorts(DataFieldEnum &df, const DeviceConfig &config) noexcept
{
#if defined(HAVE_POSIX)
  DetectSerialPorts(df);
#elif defined(_WIN32)
  FillDefaultSerialPorts(df);
#endif

  if (config.port_type == DeviceConfig::PortType::SERIAL)
    SetPort(df, config.port_type, config.path);
}

void
SetBluetoothPort(DataFieldEnum &df, DeviceConfig::PortType type,
                 const TCHAR *bluetooth_mac) noexcept
{
  assert(bluetooth_mac != nullptr);

  if (!df.Set(bluetooth_mac)) {
    const TCHAR *name = nullptr;
#ifdef ANDROID
    if (bluetooth_helper != nullptr)
      name = bluetooth_helper->GetNameFromAddress(Java::GetEnv(),
                                                  bluetooth_mac);
#endif
    df.Set(AddPort(df, type, bluetooth_mac, name));
  }
}

static void
FillAndroidBluetoothPorts(DataFieldEnum &df,
                          const DeviceConfig &config) noexcept
{
#ifdef ANDROID
  if (bluetooth_helper == nullptr)
    return;

  JNIEnv *env = Java::GetEnv();
  // list() returns an array of strings, 3 for each device, giving
  //   mac address
  //   name
  //   type - either "BLE" or "CLASSIC"
  static constexpr jsize BLUETOOTH_LIST_STRIDE = 3;
  const auto bonded = bluetooth_helper->GetBondedList(env);
  if (bonded) {
    jsize n = env->GetArrayLength(bonded) / BLUETOOTH_LIST_STRIDE;
    for (jsize i = 0; i < n; ++i) {
      Java::String address{env, (jstring)env->GetObjectArrayElement(bonded, i * BLUETOOTH_LIST_STRIDE)};
      if (!address)
        continue;

      const auto address2 = Java::String::GetUTFChars(env, address);

      Java::String name{env, (jstring)env->GetObjectArrayElement(bonded, i * BLUETOOTH_LIST_STRIDE + 1)};
      const auto name2 = name
        ? name.GetUTFChars()
        : nullptr;

      // TODO PortType::BLE_SENSOR?
      Java::String devType{env, (jstring)env->GetObjectArrayElement(bonded, i * BLUETOOTH_LIST_STRIDE + 2)};
      const DeviceConfig::PortType portType = devType != nullptr &&
        strcmp("BLE", Java::String::GetUTFChars(env, devType).c_str()) == 0
        ? DeviceConfig::PortType::BLE_HM10
        : DeviceConfig::PortType::RFCOMM;
      AddPort(df, portType, address2.c_str(), name2.c_str());
    }
  }
#endif

  if (config.UsesBluetoothMac() &&
      !config.bluetooth_mac.empty())
    SetBluetoothPort(df, config.port_type, config.bluetooth_mac);
}

static void
FillAndroidUsbSerialPorts(DataFieldEnum &df,
                          const DeviceConfig &config) noexcept
{
#ifdef ANDROID
  JNIEnv *env = Java::GetEnv();
  Java::LocalRef<jobjectArray> list{env, UsbSerialHelper::list(env)};
  if (!list)
    return;

  jsize n = env->GetArrayLength(list) / 2;
  for (jsize i = 0; i < n; ++i) {
    Java::String j_id{env, (jstring)env->GetObjectArrayElement(list, i * 2)};
    if (!j_id)
      continue;

    Java::String j_name{env, (jstring)env->GetObjectArrayElement(list, i * 2 + 1)};
    if (!j_name)
      continue;

    const auto id = j_id.ToString();
    const auto name = j_name.ToString();

    char display_string[256];
    StringFormat(display_string, sizeof(display_string),
                 "USB: %s", name.c_str());

    AddPort(df, DeviceConfig::PortType::ANDROID_USB_SERIAL,
            id.c_str(), display_string);
  }

  if (config.port_type == DeviceConfig::PortType::ANDROID_USB_SERIAL &&
      !config.path.empty())
    SetPort(df, DeviceConfig::PortType::ANDROID_USB_SERIAL, config.path);
#endif
}

static void
FillAndroidIOIOPorts(DataFieldEnum &df, const DeviceConfig &config) noexcept
{
#if defined(ANDROID)
  df.EnableItemHelp(true);

  TCHAR tempID[4];
  TCHAR tempName[15];
  for (unsigned i = 0; i < AndroidIOIOUartPort::getNumberUarts(); i++) {
    StringFormatUnsafe(tempID, _T("%u"), i);
    StringFormat(tempName, sizeof(tempName), _T("IOIO UART %u"), i);
    unsigned id = AddPort(df, DeviceConfig::PortType::IOIOUART,
                          tempID, tempName,
                          AndroidIOIOUartPort::getPortHelp(i));
    if (config.port_type == DeviceConfig::PortType::IOIOUART &&
        config.ioio_uart_id == i)
      df.Set(id);
  }
#endif
}

void
FillPorts(DataFieldEnum &df, const DeviceConfig &config) noexcept
{
  FillPortTypes(df, config);
  FillSerialPorts(df, config);
  FillAndroidBluetoothPorts(df, config);
  FillAndroidUsbSerialPorts(df, config);
  FillAndroidIOIOPorts(df, config);
}

void
SetPort(DataFieldEnum &df, const DeviceConfig &config) noexcept
{
  switch (config.port_type) {
  case DeviceConfig::PortType::DISABLED:
  case DeviceConfig::PortType::AUTO:
  case DeviceConfig::PortType::INTERNAL:
  case DeviceConfig::PortType::DROIDSOAR_V2:
  case DeviceConfig::PortType::NUNCHUCK:
  case DeviceConfig::PortType::I2CPRESSURESENSOR:
  case DeviceConfig::PortType::IOIOVOLTAGE:
  case DeviceConfig::PortType::TCP_CLIENT:
  case DeviceConfig::PortType::TCP_LISTENER:
  case DeviceConfig::PortType::UDP_LISTENER:
  case DeviceConfig::PortType::PTY:
  case DeviceConfig::PortType::RFCOMM_SERVER:
  case DeviceConfig::PortType::GLIDER_LINK:
    break;

  case DeviceConfig::PortType::SERIAL:
  case DeviceConfig::PortType::ANDROID_USB_SERIAL:
    SetPort(df, config.port_type, config.path);
    return;

  case DeviceConfig::PortType::BLE_SENSOR:
  case DeviceConfig::PortType::BLE_HM10:
  case DeviceConfig::PortType::RFCOMM:
    SetBluetoothPort(df, config.port_type, config.bluetooth_mac);
    return;

  case DeviceConfig::PortType::IOIOUART:
    StaticString<16> buffer;
    buffer.UnsafeFormat(_T("%d"), config.ioio_uart_id);
    df.Set(buffer);
    return;
  }

  for (unsigned i = 0; port_types[i].label != nullptr; i++) {
    if (port_types[i].type == config.port_type) {
      df.Set(port_types[i].label);
      break;
    }
  }
}

DeviceConfig::PortType
GetPortType(const DataFieldEnum &df) noexcept
{
  const unsigned port = df.GetValue();

  if (port < num_port_types)
    return port_types[port].type;

  return (DeviceConfig::PortType)(port >> 16);
}
