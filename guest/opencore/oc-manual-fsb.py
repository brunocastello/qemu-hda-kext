#!/usr/bin/python
# -*- coding: utf-8 -*-
"""
Switch OpenCore PlatformInfo from Automatic to manual mode for MacPro5,1.

The manual DataHub/PlatformNVRAM/SMBIOS sections reproduce what OpenCore's
Automatic mode generates for MacPro5,1 (OpenCorePkg 0.6.6 AppleModels
database MP51.yaml + OcMacInfoLib/OpenCorePlatform.c), using the serial,
MLB, ROM and UUID from PlatformInfo/Generic. The one intended difference is
a fixed DataHub/FSBFrequency:

  QEMU TCG on an ARM64 host runs the guest TSC at exactly 1 GHz and reports
  a bus ratio of 4. The 10.6 x86_64 kernel panics ("commpage no match") when
  its TSC frequency is <= SLOW_TSC_THRESHOLD (1000067800 Hz), so the true
  FSB (250 MHz) cannot be used. 250050000 Hz gives a kernel TSC of
  1000200000 Hz: no panic, guest clock 0.02% slow (was 25% with the
  auto-detected FSB of OpenCore 0.6.6).

Usage: python oc-manual-fsb.py /Volumes/EFI-LEGACY/EFI/OC/config.plist
Runs with the Python 2.6 of Mac OS X 10.6 (and with Python 3 for testing).
"""
import struct
import sys
import plistlib

FSB_FREQUENCY = 250050000

# OpenCorePkg 0.6.6 AppleModels/DataBase/MacPro/MP51.yaml
MP51 = {
    'BIOSVersion': '144.0.0.0.0',
    'BIOSReleaseDate': '04/12/2019',
    'SystemVersion': '1.2',
    'SystemSKUNumber': 'System SKU#',
    'SystemFamily': 'MacPro',
    'BoardProduct': 'Mac-F221BEC8',
    'BoardVersion': 'MacPro5,1',
    'BoardAssetTag': '',
    'BoardType': 0xB,
    'BoardLocationInChassis': 'Part Component',
    'ChassisType': 0x7,
    'ChassisAssetTag': '',
    'FirmwareFeatures': 0xE80FE137,
    'FirmwareFeaturesMask': 0xFF1FFF3F,
    'SmcRevision': b'\x01\x39\x0f\x00\x00\x11',
    'SmcBranch': b'k5\x00\x00\x00\x00\x00\x00',
    'SmcPlatform': b'k5\x00\x00\x00\x00\x00\x00',
    # OcSmbiosGetSmcVersion() applied to SmcRevision (what Automatic writes)
    'SmcVersion': b'1.39F0011' + b'\x00' * 7,
}

FW_FEATURE_SUPPORTS_CSM_LEGACY_MODE = 0x00000001
FW_FEATURE_SUPPORTS_UEFI_WINDOWS_BOOT = 0x20000000
PT_FEATURE_HAS_SOLDERED_SYSTEM_MEMORY = 0x00000002

if hasattr(plistlib, 'readPlist') and sys.version_info[0] == 2:
    def load(path):
        return plistlib.readPlist(path)

    def save(obj, path):
        plistlib.writePlist(obj, path)

    def data(raw):
        return plistlib.Data(raw)

    def raw(value):
        return value.data if isinstance(value, plistlib.Data) else value
else:
    def load(path):
        f = open(path, 'rb')
        try:
            return plistlib.load(f)
        finally:
            f.close()

    def save(obj, path):
        f = open(path, 'wb')
        try:
            plistlib.dump(obj, f, sort_keys=False)
        finally:
            f.close()

    def data(raw):
        return raw

    def raw(value):
        return value


def fail(msg):
    sys.stderr.write('ERROR: %s\nconfig.plist was not changed.\n' % msg)
    sys.exit(1)


def main():
    if len(sys.argv) != 2:
        fail('usage: %s config.plist' % sys.argv[0])
    path = sys.argv[1]
    cfg = load(path)

    pi = cfg.get('PlatformInfo')
    if pi is None or 'Generic' not in pi:
        fail('no PlatformInfo/Generic section')
    if not pi.get('Automatic', False):
        fail('PlatformInfo/Automatic is already false (already converted?)')

    g = pi['Generic']
    product = g.get('SystemProductName', '')
    if product != 'MacPro5,1':
        fail('SystemProductName is %r; this script only knows MacPro5,1' % product)

    serial = g.get('SystemSerialNumber', '')
    mlb = g.get('MLB', '')
    uuid = g.get('SystemUUID', '')
    rom = raw(g.get('ROM', b''))
    for name, value in (('SystemSerialNumber', serial), ('MLB', mlb), ('SystemUUID', uuid)):
        if not value or value == 'OEM':
            fail('Generic/%s is %r; cannot convert' % (name, value))
    if len(rom) != 6:
        fail('Generic/ROM must be 6 bytes (got %d)' % len(rom))

    vendor = 'Acidanthera' if g.get('SpoofVendor', False) else ''
    bios_version = MP51['BIOSVersion']
    if g.get('MaxBIOSVersion', False):
        bios_version = '9999.999.999.999.999'

    fw = MP51['FirmwareFeatures']
    fw_mask = MP51['FirmwareFeaturesMask']
    if g.get('AdviseWindows', False):
        extra = FW_FEATURE_SUPPORTS_CSM_LEGACY_MODE | FW_FEATURE_SUPPORTS_UEFI_WINDOWS_BOOT
        fw |= extra
        fw_mask |= extra
    fw_data = struct.pack('<Q', fw)
    fw_mask_data = struct.pack('<Q', fw_mask)

    # MP51 has no PlatformFeature in the database: Automatic leaves it unset
    # unless SystemMemoryStatus forces it. 0xFFFFFFFF = "do not set".
    platform_feature = 0xFFFFFFFF
    memory_status = g.get('SystemMemoryStatus', 'Auto')
    if memory_status == 'Upgradable':
        platform_feature = 0
    elif memory_status == 'Soldered':
        platform_feature = PT_FEATURE_HAS_SOLDERED_SYSTEM_MEMORY

    pi['DataHub'] = {
        'PlatformName': 'platform',
        'SystemProductName': product,
        'SystemSerialNumber': serial,
        'SystemUUID': uuid,
        'BoardProduct': MP51['BoardProduct'],
        'BoardRevision': data(b'\x00'),
        'DevicePathsSupported': 1,
        'SmcRevision': data(MP51['SmcRevision']),
        'SmcBranch': data(MP51['SmcBranch']),
        'SmcPlatform': data(MP51['SmcPlatform']),
        'StartupPowerEvents': 0,
        'InitialTSC': 0,
        'FSBFrequency': FSB_FREQUENCY,
        'ARTFrequency': 0,
    }
    pi['PlatformNVRAM'] = {
        'BID': MP51['BoardProduct'],
        'MLB': mlb,
        'ROM': data(rom),
        'SystemUUID': uuid,
        'FirmwareFeatures': data(fw_data),
        'FirmwareFeaturesMask': data(fw_mask_data),
    }
    pi['SMBIOS'] = {
        'BIOSVendor': vendor,
        'BIOSVersion': bios_version,
        'BIOSReleaseDate': MP51['BIOSReleaseDate'],
        'SystemManufacturer': vendor,
        'SystemProductName': product,
        'SystemVersion': MP51['SystemVersion'],
        'SystemSerialNumber': serial,
        'SystemUUID': uuid,
        'SystemSKUNumber': MP51['SystemSKUNumber'],
        'SystemFamily': MP51['SystemFamily'],
        'BoardManufacturer': vendor,
        'BoardProduct': MP51['BoardProduct'],
        'BoardVersion': MP51['BoardVersion'],
        'BoardSerialNumber': mlb,
        'BoardAssetTag': MP51['BoardAssetTag'],
        'BoardType': MP51['BoardType'],
        'BoardLocationInChassis': MP51['BoardLocationInChassis'],
        'ChassisManufacturer': vendor,
        'ChassisType': MP51['ChassisType'],
        'ChassisVersion': MP51['BoardProduct'],
        'ChassisSerialNumber': serial,
        'ChassisAssetTag': MP51['ChassisAssetTag'],
        'PlatformFeature': platform_feature,
        'ProcessorType': g.get('ProcessorType', 0),
        'FirmwareFeatures': data(fw_data),
        'FirmwareFeaturesMask': data(fw_mask_data),
        'SmcVersion': data(MP51['SmcVersion']),
    }
    pi['Automatic'] = False

    save(cfg, path)
    print('Converted PlatformInfo to manual mode (MacPro5,1, FSBFrequency %d).' % FSB_FREQUENCY)


if __name__ == '__main__':
    main()
