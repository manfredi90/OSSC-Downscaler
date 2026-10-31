#ifndef DS_SETTINGS_H_
#define DS_SETTINGS_H_

// Last sector of the secondary slot (0x80000-0xFFFFF). The only flash
// location this firmware ever writes.
#define DS_SETTINGS_FLASH_ADDR  0x000F0000

int ds_settings_load();
int ds_settings_save();
int ds_settings_reset();

#endif
