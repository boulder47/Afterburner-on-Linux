// afterburner_bar_dump.m — macOS helper to dump Apple Afterburner PCI BAR registers.
//
// This tool:
//   1. Finds the Apple Afterburner device in IOKit (vendor 0x106b, device 0x205)
//   2. Maps its PCI BARs using IOService::getReg / IODeviceMemory
//   3. Dumps all readable registers from BAR0 and BAR1 to a text file
//
// Build:
//   clang -framework IOKit -framework CoreFoundation afterburner_bar_dump.m -o afterburner_bar_dump
//
// Usage:
//   sudo ./afterburner_bar_dump [output_file]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/pci/IOPCIDevice.h>

#define AB_VENDOR 0x106b
#define AB_DEVICE 0x205

static void dump_bar(io_service_t service, int bar_index, const char *bar_name, FILE *out) {
    // Get the BAR size and mapping
    unsigned long size = 0;
    io_map_t map = 0;
    
    // Try to get the register mapping for this BAR
    kern_return_t kr = IOServiceGetReg(service, bar_index, &map, &size);
    if (kr != KERN_SUCCESS || map == 0) {
        fprintf(out, "BAR%d (%s): Could not map (kr=%d)\n", bar_index, bar_name, kr);
        return;
    }
    
    fprintf(out, "\n=== BAR%d (%s) — size %lu bytes ===\n", bar_index, bar_name, size);
    
    // Dump all 32-bit registers
    uint32_t *regs = (uint32_t *)map;
    int num_regs = size / sizeof(uint32_t);
    
    for (int i = 0; i < num_regs; i++) {
        uint32_t val = regs[i];
        
        // Only print non-zero and non-0xFFFFFFFF values to reduce noise,
        // but always print the first 64 registers for context.
        if (i < 64 || (val != 0 && val != 0xFFFFFFFF)) {
            fprintf(out, "  BAR%d+0x%05X = 0x%08X\n", bar_index, i * 4, val);
        }
    }
    
    // Unmap the register space
    IOServiceCloseMapping(service, map);
}

int main(int argc, char **argv) {
    const char *output_file = (argc > 1) ? argv[1] : "bar_dump.txt";
    FILE *out = fopen(output_file, "w");
    if (!out) {
        perror("fopen");
        return 1;
    }
    
    fprintf(out, "=== Apple Afterburner BAR Dump ===\n");
    fprintf(out, "Generated: %s\n", ctime(&((struct tm *)gmtime(time(NULL)))->tm_sec * 0)); // Simplified
    
    // Open IOKit
    io_iterator_t iterator;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMasterPortDefault,
                                                     IOServiceMatching("AppleAfterburner"),
                                                     &iterator);
    if (kr != KERN_SUCCESS) {
        fprintf(out, "IOServiceGetMatchingServices failed: %d\n", kr);
        fclose(out);
        return 1;
    }
    
    io_service_t service = IOIteratorNext(iterator);
    IOIteratorReset(iterator);
    IOObjectRelease(iterator);
    
    if (service == MACH_PORT_NULL) {
        fprintf(out, "No AppleAfterburner device found in IOKit.\n");
        fclose(out);
        return 1;
    }
    
    // Get basic device info
    CFTypeRef vendor_id = IORegistryEntryCreateCFProperty(service, 
                                                           CFSTR("vendor-id"), 
                                                           kCFAllocatorDefault, 0);
    CFTypeRef device_id = IORegistryEntryCreateCFProperty(service, 
                                                           CFSTR("device-id"), 
                                                           kCFAllocatorDefault, 0);
    
    if (vendor_id && device_id) {
        int vid = 0, did = 0;
        CFNumberGetValue((CFNumberRef)vendor_id, kCFNumberIntType, &vid);
        CFNumberGetValue((CFNumberRef)device_id, kCFNumberIntType, &did);
        fprintf(out, "Device: vendor=0x%04X device=0x%04X\n", vid, did);
        
        if (vendor_id) CFRelease(vendor_id);
        if (device_id) CFRelease(device_id);
    }
    
    // Dump BAR0 and BAR1
    dump_bar(service, 0, "BAR0", out);
    dump_bar(service, 1, "BAR1", out);
    
    // Also try to get any IODMACommand-related properties
    fprintf(out, "\n=== IOKit Properties ===\n");
    CFMutableDictionaryRef props = IORegistryEntryCreateCFProperties(service, 
                                                                      kCFAllocatorDefault, 0);
    if (props) {
        CFIndex num_keys = CFDictionaryGetCount(props);
        const void *keys[128], *values[128];
        
        // Limit to first 128 properties to avoid overflow
        int n = (num_keys > 128) ? 128 : (int)num_keys;
        if (CFDictionaryGetKeysAndValues(props, keys, values)) {
            for (int i = 0; i < n; i++) {
                CFStringRef key_str = NULL;
                if (CFGetTypeID(keys[i]) == CFStringGetTypeID()) {
                    char key_buf[256];
                    if (CFStringGetCString((CFStringRef)keys[i], key_buf, sizeof(key_buf), kCFStringEncodingUTF8)) {
                        fprintf(out, "  Property: %s\n", key_buf);
                        
                        // Try to print simple values
                        CFTypeRef val = values[i];
                        if (CFGetTypeID(val) == CFNumberGetTypeID()) {
                            int64_t num_val;
                            if (CFNumberGetValue((CFNumberRef)val, kCFNumberInt64Type, &num_val)) {
                                fprintf(out, "    Value: %lld (0x%llX)\n", num_val, num_val);
                            }
                        } else if (CFGetTypeID(val) == CFStringGetTypeID()) {
                            char val_buf[256];
                            if (CFStringGetCString((CFStringRef)val, val_buf, sizeof(val_buf), kCFStringEncodingUTF8)) {
                                fprintf(out, "    Value: %s\n", val_buf);
                            }
                        } else {
                            fprintf(out, "    Value: (%s)\n", 
                                    CFGetTypeID(val) == CFDataGetTypeID() ? "data" : "complex");
                        }
                    }
                }
            }
        }
        CFRelease(props);
    }
    
    IOObjectRelease(service);
    fclose(out);
    
    printf("BAR dump written to: %s\n", output_file);
    return 0;
}
