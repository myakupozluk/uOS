#include <u_kernel/filesystem/fat/fat.h>
#include <u_kernel/memory/u_memory.h>
#include <u_kernel/util/random/u_rand.h>
#include <u_kernel/util/util.h>
#include <u_kernel/filesystem/vfs/vfs.h>
#include <u_kernel/objects/fsi/fsi.h>
#include <u_kernel/util/u_cstr_util.h>

// os driver instance handling
typedef struct fat32_instance
{
    uobject_ref storage_device;
    partition_info partition;
    fat32_bpb bpb;
    fat32_FSInfo FSInfo;

    uint64_t fat1_first_lba;
    uint64_t fat2_first_lba;
    uint32_t data_first_lba;
    uint32_t data_cluster_count;

    struct fat32_instance* next;
} fat32_instance;

fat32_instance* instances = nullptr;

void fat32_create_new_instance(fat32_instance* instance_info){

    if (instances == nullptr) {
        instances = (fat32_instance*)kmalloc(sizeof(fat32_instance));
        memcpy(instances, instance_info, sizeof(fat32_instance));
        instances->next = nullptr;
        return;
    }

    fat32_instance* current = instances;

    if(current != nullptr){
        // find end of chain
        while(current->next != nullptr){
            current = current->next;
        }

        current->next = (fat32_instance*)kmalloc(sizeof(fat32_instance));
        current = current->next;
        memcpy(current, instance_info, sizeof(fat32_instance));
        current->next = nullptr;
        return;

    }else{
        current = (fat32_instance*)kmalloc(sizeof(fat32_instance));
        memcpy(current, instance_info, sizeof(fat32_instance));
        current->next = nullptr;
        return;
    }
}

// returns nullptr if not found
fat32_instance* fat32_find_instance(partition_info partition){
    fat32_instance* current = instances;
    while(current != nullptr){
        if(!memcmp(&current->partition, &partition, sizeof(partition_info))){
            return current;
        }
        current = current->next;
    }
    return nullptr; // not found
}

// driver

uos_result check_fat32_partition(partition_info partition, uobject_ref storage_device){
    udevice_storage_function_pointers* stor_device = _open_storage_device_(storage_device);

    if(stor_device == nullptr) return FAIL;

    // read BPB
    fat32_bpb* bpb = kmalloc(sizeof(fat32_bpb));

    if(stor_device->read(partition.start_lba + 0, 1, (uint8_t*)(bpb)) == FAIL){
        udbP("FAT GPT ERROR: Failed to read FAT header!");
        kfree(bpb);
        if(uobject_close_object(storage_device) == FAIL){
            udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
        }
        return UFS_FAIL;
    }

    // check head
    uos_result check = SUCCESS;
    uint8_t spc = bpb->sector_per_cluster;
    if(bpb->BS_Sign != 0xAA55){
        check = FAIL;
    }
    else if(bpb->BPB_FSVer != 0ULL){
        check = FAIL;
    }
    else if(read_uint16_alignment_safe(&(bpb->bytes_per_sector)) != 512ULL){
        check = FAIL;
    }
    else if(read_uint16_alignment_safe(&(bpb->rootEntCount)) != 0){
        check = FAIL;
    }
    else if(read_uint16_alignment_safe(&(bpb->totSec16)) != 0){
        check = FAIL;
    }
    else if(read_uint16_alignment_safe(&(bpb->BPB_FATSz16)) != 0){
        check = FAIL;
    }
    else if(bpb->BPB_FATSz32 == 0){
        check = FAIL;
    }
    else if(bpb->number_of_fats == 0){
        check = FAIL;
    }
    else if(bpb->sector_per_cluster > 128ULL){
        check = FAIL;
    }
    else if (spc == 0 || (spc & (spc - 1)) != 0){
        check = FAIL;
    }
    else if (bpb->reserved_sector_count == 0) {
        check = FAIL;
    }
    else if (bpb->TotSec32 == 0) {
        check = FAIL;
    }
    else if (bpb->BPB_RootClus < 2) {
        check = FAIL;
    }
    
    kfree(bpb);
    if(uobject_close_object(storage_device) == FAIL){
        udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
        return UFS_FAIL;
    }
    return check;
}

uos_result create_fat32_partition(partition_info partition, uobject_ref storage_device){
    udevice_storage_function_pointers* stor_device = _open_storage_device_(storage_device);

    if(stor_device == nullptr) return FAIL;

    // create BPB
    fat32_bpb bpb;
    memset(&bpb, 0x0, sizeof(fat32_bpb));
    bpb.jmpBoot[0] = 0xEB;
    bpb.jmpBoot[1] = 0x58;
    bpb.jmpBoot[2] = 0x90;
    bpb.BPB_SecPerTrk = 63;
    bpb.BPB_NumHeads = 255;
    bpb.BPB_HiddSec = partition.start_lba;  
    memcpy(bpb.OEMName, "MSDOS5.0", 8);
    write_uint16_alignment_safe(&bpb.bytes_per_sector, stor_device->get_lba_size());
    uint16_t safe_holded_bytes_per_sector = stor_device->get_lba_size();
    bpb.sector_per_cluster = 8; // 4kb
    bpb.reserved_sector_count = 32;
    bpb.number_of_fats = 2;
    bpb.BPB_Media = 0xF8;
    bpb.TotSec32 = partition.lba_count;
    uint64_t div = (bpb.sector_per_cluster * (safe_holded_bytes_per_sector / 4)) + bpb.number_of_fats; 
    bpb.BPB_FATSz32 = (partition.lba_count - (bpb.reserved_sector_count) + div - 1) / div; // (total - reserved) / ((sector_per_cluster * (safe_holded_bytes_per_sector / 8)) + 2) // +2 for 2 fat tables
    
    // fix 2 dummy cluster entries
    while(true){
        uint64_t data_sector_count = bpb.TotSec32 - bpb.reserved_sector_count - (bpb.BPB_FATSz32 * bpb.number_of_fats);
        uint64_t cluster_count = data_sector_count / bpb.sector_per_cluster;
        uint64_t fat_mapped_cluster_count = (bpb.BPB_FATSz32 * safe_holded_bytes_per_sector) / 4;
        if(fat_mapped_cluster_count < cluster_count + 2){
            bpb.BPB_FATSz32++;
        }else{
            break;
        }
    }

    bpb.BPB_ExtFlags = 0x0000;
    bpb.BPB_FSVer = 0x0000;
    bpb.BPB_RootClus = 2; // 0 & 1 is not healty to use
    bpb.BPB_FSInfo = 1; // 2nd sector
    bpb.BPB_BkBootSec = 0; // self
    bpb.BS_DriveNum = 0x80;
    bpb.BS_BootSig = 0x29;
    write_uint32_alignment_safe(&bpb.BS_VolID, hardware_rng32()); // we dont have rtc yet so just pass some random number
    memcpy(bpb.BS_VolLab, "uOS Volume", 11);
    memcpy(bpb.BS_FilSysType, "FAT32   ", 8);
    bpb.BS_Sign = 0xAA55;

    // write bpb to first lba in partition
    if(stor_device->write(partition.start_lba, 1, (uint8_t*)&bpb) == FAIL){
        udbP("FAT GPT ERROR: Failed to write BPB header!");
        if(uobject_close_object(storage_device) == FAIL){
            udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
        }
        return FAIL;
    }

    // create FSInfo
    fat32_FSInfo fsi;
    memset(&fsi, 0x0, sizeof(fat32_FSInfo));
    fsi.FSI_LeadSig = 0x41615252;
    fsi.FSI_StrucSig = 0x61417272;
    fsi.FSI_Free_Count = FAT32_FSINFO_UNKNOWN;
    fsi.FSI_Nxt_Free = FAT32_FSINFO_UNKNOWN;
    fsi.FSI_TrailSig = 0xAA550000;

    // write FSInfo
    if(stor_device->write(partition.start_lba + bpb.BPB_FSInfo, 1, (uint8_t*)&fsi) == FAIL){
        udbP("FAT GPT ERROR: Failed to write FSInfo header!");
        if(uobject_close_object(storage_device) == FAIL){
            udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
        }
        return FAIL;
    }

    // create & write fat tables
    uint64_t fat1_lba = partition.start_lba + bpb.reserved_sector_count;
    uint64_t fat2_lba = fat1_lba + bpb.BPB_FATSz32;
    uint64_t fat_entries_per_page = safe_holded_bytes_per_sector / 4;

    uint32_t* fat_page = kmalloc(safe_holded_bytes_per_sector);
    memset(fat_page, 0x0, safe_holded_bytes_per_sector);

    // write empty clusters first
    for(size_t i = 1; i < bpb.BPB_FATSz32; i++){
        // fat1
        if(stor_device->write(fat1_lba + i, 1, (uint8_t*)fat_page) == FAIL){
            udbP("FAT GPT ERROR: Failed to write FAT1!");
            kfree(fat_page);
            if(uobject_close_object(storage_device) == FAIL){
                udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
            }
            return FAIL;
        }

        // fat2
        if(stor_device->write(fat2_lba + i, 1, (uint8_t*)fat_page) == FAIL){
            udbP("FAT GPT ERROR: Failed to write FAT2!");
            kfree(fat_page);
            if(uobject_close_object(storage_device) == FAIL){
                udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
            }
            return FAIL;
        }
    }

    // then minimum required clusters
    fat_page[0] = 0x0FFFFFF8;
    fat_page[1] = 0xFFFFFFFF;
    fat_page[2] = 0x0FFFFFFF; // root cluster

    // fat1
    if(stor_device->write(fat1_lba + 0, 1, (uint8_t*)fat_page) == FAIL){
        udbP("FAT GPT ERROR: Failed to write FAT1!");
        kfree(fat_page);
        if(uobject_close_object(storage_device) == FAIL){
            udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
        }
        return FAIL;
    }

    // fat2
    if(stor_device->write(fat2_lba + 0, 1, (uint8_t*)fat_page) == FAIL){
        udbP("FAT GPT ERROR: Failed to write FAT2!");
        kfree(fat_page);
        if(uobject_close_object(storage_device) == FAIL){
            udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
        }
        return FAIL;
    }
    kfree(fat_page);

    // fill root cluster with zeros
    uint32_t* zeroed_root_clus = kmalloc(safe_holded_bytes_per_sector * bpb.sector_per_cluster);
    memset(zeroed_root_clus, 0x0, safe_holded_bytes_per_sector * bpb.sector_per_cluster);

    uint64_t first_cluster_lba = partition.start_lba + bpb.reserved_sector_count + (bpb.BPB_FATSz32 * bpb.number_of_fats);

    if(stor_device->write(first_cluster_lba, bpb.sector_per_cluster, (uint8_t*)zeroed_root_clus) == FAIL){
        udbP("FAT GPT ERROR: Failed to write RootCluster!");
        kfree(zeroed_root_clus);
        if(uobject_close_object(storage_device) == FAIL){
            udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
        }
        return FAIL;
    }
    kfree(zeroed_root_clus);

    if(uobject_close_object(storage_device) == FAIL){
        udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
    }
    return SUCCESS;
}

#define FAT32_CLUSTER_FREE (0x00000000)
#define FAT32_NEW_CLUSTER FAT32_CLUSTER_FREE
#define FAT32_CLUSTER_RESERVED (0x00000001)

#define FAT32_CLUSTER_ALLOCATED_MIN (0x00000002)
#define FAT32_CLUSTER_ALLOCATED_MAX (0x0FFFFFF6)
#define FAT32_CHECK_IS_CLUSTER_ALLOCATED(cluster_value) ((cluster_value >= FAT32_CLUSTER_ALLOCATED_MIN) && (cluster_value <= FAT32_CLUSTER_ALLOCATED_MAX))

#define FAT32_CLUSTER_BAD_CLUSTER (0x0FFFFFF7)
#define FAT32_CLUSTER_CHAIN_END_MIN (0x0FFFFFF8)
#define FAT32_CLUSTER_CHAIN_END_MAX (0x0FFFFFFF)
#define FAT32_CHECK_IS_CLUSTER_END(cluster_value) ((cluster_value >= FAT32_CLUSTER_CHAIN_END_MIN) && (cluster_value <= FAT32_CLUSTER_CHAIN_END_MAX))

typedef uint32_t fat32_cluster_entry;

#define FAT32_ATTR_READ_ONLY_BIT ONEBIT(0)
#define FAT32_ATTR_HIDDEN_BIT ONEBIT(1)
#define FAT32_ATTR_SYSTEM_BIT ONEBIT(2)
#define FAT32_ATTR_VOLUME_ID_BIT ONEBIT(3)
#define FAT32_ATTR_DIRECTORY_BIT ONEBIT(4)
#define FAT32_ATTR_ARCHIVE_BIT ONEBIT(5)
#define FAT32_ATTR_LONG_FILE_NAME (0x0F)

typedef struct
{
    char DIR_Name[11]; // SFN
    uint8_t DIR_Attr;
    uint8_t DIR_NTRes; // not used (0)

    // time is not present so set to 0
    uint8_t DIR_CrtTimeTenth;
    uint16_t DIR_CrtTime;
    uint16_t DIR_CrtDate;
    uint16_t DIR_LstAccDate;
    uint16_t DIR_FstClusHI; // used in fat32
    uint16_t DIR_WrtTime;
    uint16_t DIR_WrtDate;

    uint16_t DIR_FstClusLO;
    uint32_t DIR_FileSize;
} __attribute__((packed)) fat32_dir_entry;
_Static_assert(sizeof(fat32_dir_entry) == 32, "fat32_dir_entry must be 32 bytes");
#define FAT32_DIR_ENTRY_FREE (0xE5)
#define FAT32_DIR_ENTRY_FREE_START (0x00)

typedef struct
{
    uint8_t LDIR_Ord; // id of lfn entry in chain
    wide_char_t LDIR_Name1[5];
    uint8_t LDIR_Attr;
    uint8_t LDIR_Type;
    uint8_t LDIR_Chksum;
    wide_char_t LDIR_Name2[6];
    uint16_t LDIR_FstClusLO; // always 0
    wide_char_t LDIR_Name3[2];
} __attribute__((packed)) fat32_long_dir_entry;
_Static_assert(sizeof(fat32_long_dir_entry) == 32, "fat32_long_dir_entry must be 32 bytes");
#define FAT32_LFN_ID_FLAG_BEGIN ONEBIT(6) // 6th bit

uint8_t fat32_calculate_lfn_chksum(char* sfn){
    uint8_t sum = 0;
    for(int i  = 0; i < 11; i++){
        sum = (sum >> 1) + (sum << 7) + sfn[i];
    }
    return sum;
}

// buffer should have 256 wchars
size_t prepend_lfn(fat32_long_dir_entry* long_dir, wide_char_t* buffer,  size_t current_read_len){
    wide_char_t* tmp = kmalloc((256 + 1 + 13) * sizeof(wide_char_t)); // +13 for shift area
    memcpy(tmp + 13, buffer, current_read_len * sizeof(wide_char_t)); // shift to right

    // copy to begining
    memcpy(tmp + 0, long_dir->LDIR_Name1, 5 * sizeof(wide_char_t));
    memcpy(tmp + 0 + 5, long_dir->LDIR_Name2, 6 * sizeof(wide_char_t));
    memcpy(tmp + 0 + 5 + 6, long_dir->LDIR_Name3, 2 * sizeof(wide_char_t));

    // copy back to original buffer
    memcpy(buffer, tmp, (current_read_len + 13) * sizeof(wide_char_t));
    kfree(tmp);
    return current_read_len + 13;
}  

// returns last offset
// NOTE: this function puts reversly so we dont need to do it reversing in use area
size_t put_in_lfn(fat32_long_dir_entry* long_dir, const wide_char_t* buffer, size_t current_wrote_len){
    size_t total_name_len = wstrlen(buffer, 256 + 1) + 1; // +1 nullterm
    if((total_name_len - current_wrote_len) < 13){
        // short entry
        size_t write_start = 0;
        size_t write_len = (total_name_len - current_wrote_len) % 13;
        size_t wrote_len = 0;
        if(write_len >= 5){
            memcpy(long_dir->LDIR_Name1, buffer + write_start + 0, 5 * sizeof(wide_char_t));
            write_len -= 5;
            write_start += 5;
            wrote_len += 5;

            if(write_len >= 6){
                memcpy(long_dir->LDIR_Name2, buffer + write_start + 0, 6 * sizeof(wide_char_t));
                write_len -= 6;
                write_start += 6;
                wrote_len += 6;

                if(write_len >= 2){
                    memcpy(long_dir->LDIR_Name3, buffer + write_start + 0, 2 * sizeof(wide_char_t));
                    write_len -= 2;
                    write_start += 2;
                    wrote_len += 2;

                    return current_wrote_len + wrote_len;
                    
                }else{
                    memcpy(long_dir->LDIR_Name3, buffer + write_start + 0, write_len * sizeof(wide_char_t));
                    wrote_len += write_len;
                    return current_wrote_len + wrote_len;
                }
                
            }else{
                memcpy(long_dir->LDIR_Name2, buffer + write_start + 0, write_len * sizeof(wide_char_t));
                wrote_len += write_len;
                return current_wrote_len + wrote_len;
            }

        }else{
            memcpy(long_dir->LDIR_Name1, buffer + write_start + 0, write_len * sizeof(wide_char_t));
            wrote_len += write_len;
            return current_wrote_len + wrote_len;
        }
        
    }else{
        // long entry
        if((total_name_len - current_wrote_len) % 13 == 0){
            // full/aligned
            size_t write_start = total_name_len - current_wrote_len - 13;
            memcpy(long_dir->LDIR_Name1, buffer + write_start + 0, 5 * sizeof(wide_char_t));
            memcpy(long_dir->LDIR_Name2, buffer + write_start + 0 + 5, 6 * sizeof(wide_char_t));
            memcpy(long_dir->LDIR_Name3, buffer + write_start + 0 + 5 + 6, 2 * sizeof(wide_char_t));
            return current_wrote_len + 13;
        }else{
            // half/unaligned
            size_t write_len = (total_name_len - current_wrote_len) % 13;
            size_t wrote_len = 0;
            size_t write_start = (total_name_len - current_wrote_len) - write_len;

            // fill with 0xFF
            memset(long_dir->LDIR_Name1, 0xFF, 5 * sizeof(wide_char_t));
            memset(long_dir->LDIR_Name2, 0xFF, 6 * sizeof(wide_char_t));
            memset(long_dir->LDIR_Name3, 0xFF, 2 * sizeof(wide_char_t));
            if(write_len >= 5){
                memcpy(long_dir->LDIR_Name1, buffer + write_start + 0, 5 * sizeof(wide_char_t));
                write_len -= 5;
                write_start += 5;
                wrote_len += 5;

                if(write_len >= 6){
                    memcpy(long_dir->LDIR_Name2, buffer + write_start + 0, 6 * sizeof(wide_char_t));
                    write_len -= 6;
                    write_start += 6;
                    wrote_len += 6;

                    if(write_len >= 2){
                        memcpy(long_dir->LDIR_Name3, buffer + write_start + 0, 2 * sizeof(wide_char_t));
                        write_len -= 2;
                        write_start += 2;
                        wrote_len += 2;

                        return current_wrote_len + wrote_len;
                        
                    }else{
                        memcpy(long_dir->LDIR_Name3, buffer + write_start + 0, write_len * sizeof(wide_char_t));
                        wrote_len += write_len;
                        return current_wrote_len + wrote_len;
                    }
                
                }else{
                    memcpy(long_dir->LDIR_Name2, buffer + write_start + 0, write_len * sizeof(wide_char_t));
                    wrote_len += write_len;
                    return current_wrote_len + wrote_len;
                }

            }else{
                memcpy(long_dir->LDIR_Name1, buffer + write_start + 0, write_len * sizeof(wide_char_t));
                wrote_len += write_len;
                return current_wrote_len + wrote_len;
            }
        }
    }
}

uos_result fat32_check_name(const wide_char_t* name)
{
    size_t len = wstrlen(name, 256);

    if (len == 0 || len > 255) return FAIL;

    for (size_t i = 0; i < len; i++)
    {
        wide_char_t c = name[i];

        if (c < 0x20)
            return FAIL;

        switch (c)
        {
            case '"':
            case '*':
            case '/':
            case ':':
            case '<':
            case '>':
            case '?':
            case '\\':
            case '|':
                return FAIL;
        }
    }

    return SUCCESS;
}

// NOTE: Buffer should be 11 bytes
void generate_random_short_name(char* buf){
    static const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for(uint8_t i = 0; i < 11; i++){
        buf[i] = charset[hardware_rng32() % 36];
    }
}

// creates a virtual entry for root directory
void fat32_get_root_dir_virt_entry(fat32_instance* inst, fat32_dir_entry* result_entry){
    memcpy(result_entry->DIR_Name, "ROOTDIRECTR", 11);
    result_entry->DIR_Attr = FAT32_ATTR_DIRECTORY_BIT;
    result_entry->DIR_NTRes = 0x0;
    result_entry->DIR_FstClusLO = (inst->bpb.BPB_RootClus) & 0xFFFF;
    result_entry->DIR_FstClusHI = (inst->bpb.BPB_RootClus >> 16);
    result_entry->DIR_FileSize = 0;
}

typedef struct
{
    fat32_dir_entry entry;
    size_t index_in_dir_list;
    wide_char_t wide_name[256]; // 255 char + nullterm
    bool sfn_match;
}fat32_entry_result;

fat32_entry_result fat32_find_entry(fat32_instance* inst, fat32_dir_entry* parent, const wide_char_t* entry_name, char* sfn_name){
    //udbP("searching for name:");
    //udbPw(entry_name);
    fat32_entry_result result = {};
    result.entry.DIR_Name[0] = FAT32_DIR_ENTRY_FREE; // not found
    result.sfn_match = false;

    // return self if empty
    if(*entry_name == '\0' || !wstrcmp(entry_name, u"/")){
        memcpy(&(result.entry), parent, sizeof(fat32_dir_entry));
        return result;
    }

    if(!(parent->DIR_Attr & FAT32_ATTR_DIRECTORY_BIT)){
        udbP("FAT ERROR: Parent is not a directory!");
        return result;
    }

    // open device
    udevice_storage_function_pointers* stor_device = _open_storage_device_(inst->storage_device);

    if(stor_device == nullptr){
        udbP("FAT ERROR: Failed to open storage device!");
        return result;
    }

    uint64_t lba_size = stor_device->get_lba_size();
    uint64_t cluster_entry_per_lba = lba_size / sizeof(fat32_cluster_entry);
    uint64_t dir_entry_count_per_cluster = (lba_size * inst->bpb.sector_per_cluster) / sizeof(fat32_dir_entry);

    fat32_cluster_entry* cluster_table = kmalloc(lba_size); // cluster entry table (or FAT)
    fat32_dir_entry* current_table = kmalloc(lba_size * inst->bpb.sector_per_cluster); // cluster
    fat32_long_dir_entry* long_current_table = (fat32_long_dir_entry*)current_table;
    wide_char_t* current_lfn_name = kmalloc(256 * sizeof(wide_char_t)); // + nullterm


    fat32_cluster_entry current_cluster = (parent->DIR_FstClusLO | (parent->DIR_FstClusHI << 16));
    size_t long_name_len = 0;
    uint8_t current_lfn_chksum = 0;
    uint8_t expected_lfn_id = 0;

    // dir search
    while(true){
        uint32_t cluster_entry_offset_in_lba = current_cluster % cluster_entry_per_lba;

        // read cluster entry
        // calculate which lba it is in fat & read
        if(stor_device->read(inst->fat1_first_lba + (current_cluster / cluster_entry_per_lba), 1, (uint8_t*)(cluster_table)) == FAIL){
            udbP("FAT ERROR: Failed to read FAT header!");
            if(uobject_close_object(inst->storage_device) == FAIL){
                udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
            }
            kfree(cluster_table);
            kfree(current_table);
            kfree(current_lfn_name);
            return result;
        }

        // cluster dir search
        // read cluster (directory entries)
        if(stor_device->read(inst->data_first_lba + ((current_cluster - 2) * inst->bpb.sector_per_cluster), inst->bpb.sector_per_cluster, (uint8_t*)(current_table)) == FAIL){
            udbP("FAT ERROR: Failed to read directory cluster!");
            if(uobject_close_object(inst->storage_device) == FAIL){
                udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
            }
            kfree(cluster_table);
            kfree(current_table);
            kfree(current_lfn_name);
            return result;
        }
            
        for(uint32_t i = 0; i < dir_entry_count_per_cluster; i++){
            if(current_table[i].DIR_Name[0] == FAT32_DIR_ENTRY_FREE_START){
                // no any entries available further
                // udbP("FAT ERROR: Entry does not exists!");
                if(uobject_close_object(inst->storage_device) == FAIL){
                    udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
                }
                kfree(cluster_table);
                kfree(current_table);
                kfree(current_lfn_name);
                return result;
            }else if(current_table[i].DIR_Name[0] != FAT32_DIR_ENTRY_FREE){
                // check long name
                if(current_table[i].DIR_Attr == FAT32_ATTR_LONG_FILE_NAME){
                    if(long_name_len){
                        if(long_current_table[i].LDIR_Ord == expected_lfn_id && long_current_table[i].LDIR_Chksum == current_lfn_chksum){
                            long_name_len = prepend_lfn(&long_current_table[i], current_lfn_name, long_name_len);
                            expected_lfn_id--;
                        }else{
                            // possiable fs corruption
                            udbP("FAT ERROR: LFN mismatch0 found! Maybe filesystem is corrupted?");
                            if(uobject_close_object(inst->storage_device) == FAIL){
                                udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
                            }
                            kfree(cluster_table);
                            kfree(current_table);
                            kfree(current_lfn_name);
                            return result;
                        }
                    }else{
                        if(long_current_table[i].LDIR_Ord & FAT32_LFN_ID_FLAG_BEGIN){
                            long_name_len = prepend_lfn(&long_current_table[i], current_lfn_name, long_name_len);
                            current_lfn_chksum = long_current_table[i].LDIR_Chksum;
                            expected_lfn_id = (long_current_table[i].LDIR_Ord & ~FAT32_LFN_ID_FLAG_BEGIN) - 1;
                        }else{
                            // possiable fs corruption
                            udbP("FAT ERROR: LFN mismatch1 found! Maybe filesystem is corrupted?");
                            if(uobject_close_object(inst->storage_device) == FAIL){
                                udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
                            }
                            kfree(cluster_table);
                            kfree(current_table);
                            kfree(current_lfn_name);
                            return result;
                        }
                    }
                }else{
                    if(long_name_len){
                        if(current_lfn_chksum == fat32_calculate_lfn_chksum(current_table[i].DIR_Name) && expected_lfn_id == 0){
                            char* current_name = kmalloc(12 * sizeof(char));
                            memcpy(current_name, current_table[i].DIR_Name, 11);
                            current_name[11] = '\0';
                            if(!wstrcmp(current_lfn_name, entry_name) || !strcmp(current_name, sfn_name)){
                                // found!
                                memcpy(&(result.entry), &(current_table[i]), sizeof(fat32_dir_entry));
                                memcpy(result.wide_name, current_lfn_name, 256 * sizeof(wide_char_t));
                                result.index_in_dir_list = i;
                                result.sfn_match = !strcmp(current_name, sfn_name); // for knowing what is conflicting
                                if(uobject_close_object(inst->storage_device) == FAIL){
                                    udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
                                }
                                kfree(cluster_table);
                                kfree(current_table);
                                kfree(current_lfn_name);
                                kfree(current_name);
                                return result;
                            }else{
                                // not found
                                long_name_len = 0;
                                current_lfn_chksum = 0;
                                expected_lfn_id = 0;
                                kfree(current_name);
                                continue;
                            }
                        }else{
                            // possiable fs corruption
                            udbP("FAT ERROR: LFN mismatch2 found! Maybe filesystem is corrupted?");
                            if(uobject_close_object(inst->storage_device) == FAIL){
                                udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
                            }
                            kfree(cluster_table);
                            kfree(current_table);
                            kfree(current_lfn_name);
                            return result;
                        }
                    }else{
                        char* short_name = kmalloc(256 * sizeof(char));
                        char* current_name = kmalloc(12 * sizeof(char));
                        memcpy(current_name, current_table[i].DIR_Name, 11);
                        current_name[11] = '\0';
                        utf16_to_char_buf(entry_name, short_name);
                        if(!strcmp(short_name, current_name) || !strcmp(current_name, sfn_name)){
                            // found!
                            memcpy(&(result.entry), &(current_table[i]), sizeof(fat32_dir_entry));
                            char_to_utf16_buf(current_name, result.wide_name);
                            result.index_in_dir_list = i;
                            result.sfn_match = !strcmp(current_name, sfn_name); // for knowing what is conflicting
                            if(uobject_close_object(inst->storage_device) == FAIL){
                                udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
                            }
                            kfree(cluster_table);
                            kfree(current_table);
                            kfree(current_lfn_name);
                            kfree(short_name);
                            kfree(current_name);
                            return result;
                        }else{
                            // not found
                            kfree(short_name);
                            kfree(current_name);
                            continue;
                        }
                    }
                }
            }
        }
        
        // not found go to next dir entry cluster
        if(FAT32_CHECK_IS_CLUSTER_ALLOCATED(cluster_table[cluster_entry_offset_in_lba])){
            current_cluster = cluster_table[cluster_entry_offset_in_lba];
        }else{
            break; // no any more cluster left
        }

    }

    // not found
    if(uobject_close_object(inst->storage_device) == FAIL){
        udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
    }
    kfree(cluster_table);
    kfree(current_table);
    kfree(current_lfn_name);
    return result;
}

// NOTE: first_allocated_cluster can be nullptr
uos_result fat32_allocate_cluster(fat32_instance* inst, udevice_storage_function_pointers* stor_device, uint32_t chain_end_cluster, uint32_t* first_allocated_cluster,  size_t new_cluster_allocation_count, bool zero_clusters){
    if(new_cluster_allocation_count == 0){
        udbP("FAT ERROR: 0 new allocation is not allowed!");
        return FAIL;
    }

    fat32_cluster_entry* cluster_table = kmalloc(stor_device->get_lba_size()); // cluster entry table (or FAT)
    uint64_t cluster_entry_per_lba = stor_device->get_lba_size() / sizeof(fat32_cluster_entry);
    
    uint32_t current_cluster = 0; // current read cluster in buffer (THIS IS CLUSTER NUMBER NOT THE SECTOR NUMBER)
    if(inst->FSInfo.FSI_Nxt_Free != FAT32_FSINFO_UNKNOWN){
        current_cluster = (inst->FSInfo.FSI_Nxt_Free / cluster_entry_per_lba) * cluster_entry_per_lba;
    }
    size_t found_free_cluster_count = 0;
    bool hinted_cluster_is_used = false;
    uint32_t* free_clusters = kmalloc(new_cluster_allocation_count * sizeof(uint32_t));
    new_cluster_allocation_count++; // last one is for capturing last free
    while (found_free_cluster_count < new_cluster_allocation_count)
    {
        // read cluster entry
        // calculate which lba it is in fat & read
        if(stor_device->read(inst->fat1_first_lba + (current_cluster / cluster_entry_per_lba), 1, (uint8_t*)(cluster_table)) == FAIL){
            udbP("FAT ERROR: Failed to read FAT!");
            kfree(cluster_table);
            kfree(free_clusters);
            return FAIL;
        }

        // find free clusters to add to chain
        for(size_t i = 0; i < cluster_entry_per_lba; i++){
            if(cluster_table[i] == FAT32_CLUSTER_FREE){
                if(found_free_cluster_count + 1  == new_cluster_allocation_count){
                    // catch last one write it to fsinfo
                    if(((current_cluster) + i) < inst->FSInfo.FSI_Nxt_Free || hinted_cluster_is_used){
                        inst->FSInfo.FSI_Nxt_Free = (current_cluster) + i;
                        if(inst->FSInfo.FSI_Free_Count != FAT32_FSINFO_UNKNOWN){
                            inst->FSInfo.FSI_Free_Count -= new_cluster_allocation_count - 1; // last captured free is not allocated so subtract it
                        }
                        // write back to storage
                        if(stor_device->write(inst->partition.start_lba + inst->bpb.BPB_FSInfo, 1, (uint8_t*)&(inst->FSInfo)) == FAIL){
                            udbP("FAT ERROR: Failed to writeback FSInfo header!");
                            kfree(cluster_table);
                            kfree(free_clusters);
                            return FAIL;
                        }
                    }
                    found_free_cluster_count++;

                    // free clusters found
                    new_cluster_allocation_count--; // release captured last free
                    break; // now go to allocation

                }else{
                    free_clusters[found_free_cluster_count] = (current_cluster) + i;
                    if(free_clusters[found_free_cluster_count] == inst->FSInfo.FSI_Nxt_Free){
                        hinted_cluster_is_used = true;
                    }
                    found_free_cluster_count++;
                }
            }
        }

        if(found_free_cluster_count >= new_cluster_allocation_count){
            break; // go to allocation
        }

        // go next table
        current_cluster += cluster_entry_per_lba;
        if(current_cluster >= inst->data_cluster_count + 2){
            udbP("FAT ERROR: No free cluster left in volume!");
            kfree(cluster_table);
            kfree(free_clusters);
            return FAIL;
        }
    }

    uint8_t* zeroed_cluster = nullptr;
    if(zero_clusters){
        zeroed_cluster = kmalloc(stor_device->get_lba_size() * inst->bpb.sector_per_cluster);
        memset(zeroed_cluster, 0x0, stor_device->get_lba_size() * inst->bpb.sector_per_cluster);
    }

    // allocation
    uint32_t last_set_chain_member = chain_end_cluster;
    if(!FAT32_CHECK_IS_CLUSTER_ALLOCATED(last_set_chain_member)){
        last_set_chain_member = free_clusters[0];
    }
    uint32_t first_clus = last_set_chain_member;
    for(size_t i = 0; i < new_cluster_allocation_count; i++){
        if(last_set_chain_member / cluster_entry_per_lba != current_cluster / cluster_entry_per_lba){
            // current sector is not correct
            // write back current sector
            if(stor_device->write(inst->fat1_first_lba + (current_cluster / cluster_entry_per_lba), 1, (uint8_t*)(cluster_table)) == FAIL){
                udbP("FAT ERROR: Failed to writeback FAT!");
                kfree(cluster_table);
                kfree(free_clusters);
                if(zeroed_cluster != nullptr){
                    kfree(zeroed_cluster);
                }
                return FAIL;
            }
            if(stor_device->write(inst->fat2_first_lba + (current_cluster / cluster_entry_per_lba), 1, (uint8_t*)(cluster_table)) == FAIL){
                udbP("FAT ERROR: Failed to writeback FAT2!");
                kfree(cluster_table);
                kfree(free_clusters);
                if(zeroed_cluster != nullptr){
                    kfree(zeroed_cluster);
                }
                return FAIL;
            }
            // read correct sector
            current_cluster = last_set_chain_member;
            if(stor_device->read(inst->fat1_first_lba + (current_cluster / cluster_entry_per_lba), 1, (uint8_t*)(cluster_table)) == FAIL){
                udbP("FAT ERROR: Failed to read FAT!");
                kfree(cluster_table);
                kfree(free_clusters);
                if(zeroed_cluster != nullptr){
                    kfree(zeroed_cluster);
                }
                return FAIL;
            }
        }
        
        // zero clusters if required
        if(zero_clusters){
            if(stor_device->write(inst->data_first_lba + ((last_set_chain_member - 2) * inst->bpb.sector_per_cluster), inst->bpb.sector_per_cluster, (uint8_t*)(zeroed_cluster)) == FAIL){
                udbP("FAT ERROR: Failed to write directory cluster!");
                if(uobject_close_object(inst->storage_device) == FAIL){
                    udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
                }
                kfree(cluster_table);
                kfree(free_clusters);
                if(zeroed_cluster != nullptr){
                    kfree(zeroed_cluster);
                }
                return FAIL;
            }
        }

        if(i + 1 != new_cluster_allocation_count){
            // not the last cluster
            cluster_table[last_set_chain_member % cluster_entry_per_lba] = free_clusters[i]; // give next
            last_set_chain_member = free_clusters[i];
        }else{
            // last cluster
            cluster_table[last_set_chain_member % cluster_entry_per_lba] = FAT32_CLUSTER_CHAIN_END_MIN; // end
        }

    }

    // write back last current sector
    if(stor_device->write(inst->fat1_first_lba + (current_cluster / cluster_entry_per_lba), 1, (uint8_t*)(cluster_table)) == FAIL){
        udbP("FAT ERROR: Failed to writeback FAT!");
        kfree(cluster_table);
        kfree(free_clusters);
        if(zeroed_cluster != nullptr){
            kfree(zeroed_cluster);
        }
        return FAIL;
    }
    if(stor_device->write(inst->fat2_first_lba + (current_cluster / cluster_entry_per_lba), 1, (uint8_t*)(cluster_table)) == FAIL){
        udbP("FAT ERROR: Failed to writeback FAT2!");
        kfree(cluster_table);
        kfree(free_clusters);
        if(zeroed_cluster != nullptr){
            kfree(zeroed_cluster);
        }
        return FAIL;
    }

    kfree(cluster_table);
    kfree(free_clusters);
    if(zeroed_cluster != nullptr){
        kfree(zeroed_cluster);
    }
    if(first_allocated_cluster != nullptr){
        *first_allocated_cluster = first_clus;
    }
    return SUCCESS; // nothing failed
}

typedef struct
{
    size_t chain_len;
    uint32_t* chain;
}fat32_cluster_chain;

// NOTE: DO NOT FORGET TO FREE chain IN STRUCT AFTER USE!
fat32_cluster_chain fat32_get_chain(fat32_instance* inst, udevice_storage_function_pointers* stor_device, uint32_t chain_start_cluster){
    fat32_cluster_chain result;
    result.chain = nullptr;
    result.chain_len = 0;

    if(!FAT32_CHECK_IS_CLUSTER_ALLOCATED(chain_start_cluster)){
        udbP("FAT ERROR: Start chain is not valid!");
        return result;
    }

    uint64_t lba_size = stor_device->get_lba_size();
    uint64_t cluster_entry_per_lba = lba_size / sizeof(fat32_cluster_entry);

    fat32_cluster_entry* cluster_table = kmalloc(lba_size); // cluster entry table (or FAT)

    fat32_cluster_entry current_cluster = chain_start_cluster;
    
    size_t allocated_chain_count = 16;
    result.chain = kmalloc(allocated_chain_count * sizeof(uint32_t));

    while(true){
        uint32_t cluster_entry_offset_in_lba = current_cluster % cluster_entry_per_lba;

        // read cluster entry
        // calculate which lba it is in fat & read
        if(stor_device->read(inst->fat1_first_lba + (current_cluster / cluster_entry_per_lba), 1, (uint8_t*)(cluster_table)) == FAIL){
            udbP("FAT ERROR: Failed to read FAT header!");
            kfree(cluster_table);
            kfree(result.chain);
            result.chain = nullptr;
            result.chain_len = 0;
            return result;
        }
        
        // add to list
        if(result.chain_len < allocated_chain_count){
            // size is enough
            result.chain[result.chain_len] = current_cluster;
            result.chain_len++;
        }else{
            // size is not enough
            allocated_chain_count += 16;
            result.chain = kreloc(result.chain, result.chain_len * sizeof(uint32_t), allocated_chain_count * sizeof(uint32_t));
            result.chain[result.chain_len] = current_cluster;
            result.chain_len++;
        }

        // go to next
        if(FAT32_CHECK_IS_CLUSTER_ALLOCATED(cluster_table[cluster_entry_offset_in_lba])){
            current_cluster = cluster_table[cluster_entry_offset_in_lba];
        }else{
            break; // no any more cluster left
        }
    }

    kfree(cluster_table);
    return result;
}

// returns  UINT32_MAX if not found
uint32_t fat32_find_continious_free_entries(fat32_instance* inst, fat32_dir_entry parent, size_t len){
     if(!(parent.DIR_Attr & FAT32_ATTR_DIRECTORY_BIT)){
        udbP("FAT ERROR: Parent is not a directory!");
        return UINT32_MAX;
    }

    // open device
    udevice_storage_function_pointers* stor_device = _open_storage_device_(inst->storage_device);

    if(stor_device == nullptr){
        udbP("FAT ERROR: Failed to open storage device!");
        return UINT32_MAX;
    }

    uint64_t lba_size = stor_device->get_lba_size();
    uint64_t cluster_entry_per_lba = lba_size / sizeof(fat32_cluster_entry);
    uint64_t dir_entry_count_per_cluster = (lba_size * inst->bpb.sector_per_cluster) / sizeof(fat32_dir_entry);

    fat32_cluster_entry* cluster_table = kmalloc(lba_size); // cluster entry table (or FAT)
    fat32_dir_entry* current_table = kmalloc(lba_size * inst->bpb.sector_per_cluster); // cluster

    fat32_cluster_entry current_cluster = (parent.DIR_FstClusLO | (parent.DIR_FstClusHI << 16));
    uint32_t checked_cluster_count = 0;
    uint32_t free_start = UINT32_MAX;
    uint32_t continuous_free_cluster_count = 0;

    // dir search
    while(true){
        uint32_t cluster_entry_offset_in_lba = current_cluster % cluster_entry_per_lba;

        // read cluster entry
        // calculate which lba it is in fat & read
        if(stor_device->read(inst->fat1_first_lba + (current_cluster / cluster_entry_per_lba), 1, (uint8_t*)(cluster_table)) == FAIL){
            udbP("FAT ERROR: Failed to read FAT header!");
            if(uobject_close_object(inst->storage_device) == FAIL){
                udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
            }
            kfree(cluster_table);
            kfree(current_table);
            return UINT32_MAX;
        }

        // cluster dir search
        // read cluster (directory entries)
        if(stor_device->read(inst->data_first_lba + ((current_cluster - 2) * inst->bpb.sector_per_cluster), inst->bpb.sector_per_cluster, (uint8_t*)(current_table)) == FAIL){
            udbP("FAT ERROR: Failed to read directory cluster!");
            if(uobject_close_object(inst->storage_device) == FAIL){
                udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
            }
            kfree(cluster_table);
            kfree(current_table);
            return UINT32_MAX;
        }
            
        for(uint32_t i = 0; i < dir_entry_count_per_cluster; i++){
            if(current_table[i].DIR_Name[0] == FAT32_DIR_ENTRY_FREE){
                if(continuous_free_cluster_count){
                    continuous_free_cluster_count++;
                }else{
                    free_start = (checked_cluster_count * dir_entry_count_per_cluster) + i;
                    continuous_free_cluster_count++;
                }
            }else if(current_table[i].DIR_Name[0] == FAT32_DIR_ENTRY_FREE_START){
                if(dir_entry_count_per_cluster - i >= len){
                    if(continuous_free_cluster_count){
                        if(uobject_close_object(inst->storage_device) == FAIL){
                            udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
                        }
                        kfree(cluster_table);
                        kfree(current_table);
                        return free_start; // use already found free start if available
                    }else{
                        if(uobject_close_object(inst->storage_device) == FAIL){
                            udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
                        }
                        kfree(cluster_table);
                        kfree(current_table);
                        return (checked_cluster_count * dir_entry_count_per_cluster) + i; // enough space available
                    }
                }else{
                    continuous_free_cluster_count += dir_entry_count_per_cluster - i;
                    break;
                }
            }else{
                free_start = UINT32_MAX;
                continuous_free_cluster_count = 0;
            }

            if(continuous_free_cluster_count >= len){
                if(uobject_close_object(inst->storage_device) == FAIL){
                    udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
                }
                kfree(cluster_table);
                kfree(current_table);
                return free_start;
            }
        }

        // this double check is for edgecase
        if(continuous_free_cluster_count >= len){
            if(uobject_close_object(inst->storage_device) == FAIL){
                udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
            }
            kfree(cluster_table);
            kfree(current_table);
            return free_start;
        }
        
        // not found go to next dir entry cluster
        if(FAT32_CHECK_IS_CLUSTER_ALLOCATED(cluster_table[cluster_entry_offset_in_lba])){
            current_cluster = cluster_table[cluster_entry_offset_in_lba];
            checked_cluster_count++;
        }else{
            // no any more cluster left
            // try to allocate new cluster
            if((checked_cluster_count * dir_entry_count_per_cluster) <= UINT16_MAX){
                if(fat32_allocate_cluster(inst, stor_device, current_cluster, nullptr, 1, true) != SUCCESS){
                    udbP("FAT ERROR: Not enough entry left in directory! H0");
                    if(uobject_close_object(inst->storage_device) == FAIL){
                        udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
                    }
                    kfree(cluster_table);
                    kfree(current_table);
                    return UINT32_MAX;
                }
                // we dont have updated version here so we have to readback the fat cluster entries

                if(stor_device->read(inst->fat1_first_lba + (current_cluster / cluster_entry_per_lba), 1, (uint8_t*)(cluster_table)) == FAIL){
                    udbP("FAT ERROR: Failed to read FAT header!");
                    if(uobject_close_object(inst->storage_device) == FAIL){
                        udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
                    }
                    kfree(cluster_table);
                    kfree(current_table);
                    return UINT32_MAX;
                }
                current_cluster = cluster_table[cluster_entry_offset_in_lba];
                checked_cluster_count++;
            }else{
                udbP("FAT ERROR: Not enough entry left in directory! H1");
                if(uobject_close_object(inst->storage_device) == FAIL){
                    udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
                }
                kfree(cluster_table);
                kfree(current_table);
                return UINT32_MAX;
            }
        }

    }

    // not found
    udbP("FAT ERROR: This should not reach here!");
    if(uobject_close_object(inst->storage_device) == FAIL){
        udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
    }
    kfree(cluster_table);
    kfree(current_table);
    return UINT32_MAX;
}

// empty sfn tries to find a suitable random sfn name
uos_result fat32_create_dir_entry(fat32_instance* inst, fat32_entry_result parent_entry, const wide_char_t* entry_name, char* sfn_name, uint8_t attrbs, uint32_t assigned_cluster, uint32_t file_size){

    char random_sfn_name[11];
    bool use_random_sfn = false;
    if(*sfn_name == '\0'){
        generate_random_short_name(random_sfn_name);
        sfn_name = random_sfn_name;
        use_random_sfn = true;
    }
    
    // try to find a suitable sfn name
    fat32_entry_result found;
    bool filename_suitable = false;
    for(size_t i = 0; i < 8196; i++){
        found = fat32_find_entry(inst, &(parent_entry.entry), entry_name, sfn_name);
        if(!(found.entry.DIR_Name[0] == FAT32_DIR_ENTRY_FREE || found.entry.DIR_Name[0] == FAT32_DIR_ENTRY_FREE_START)){
            // found a match
            if(found.sfn_match){
                if(use_random_sfn){
                    generate_random_short_name(sfn_name);
                }else{
                    udbP("FAT ERROR: A entry with this short file name already exists!");
                    return FAIL; 
                }
            }else{
                udbP("FAT ERROR: A entry with this long file name already exists!");
                return FAIL;
            }
        }else{
            // no confliction
            filename_suitable = true;
            break;
        }
    }

    if(!filename_suitable){
        udbP("FAT ERROR: Failed to find a suitable random sfnv!");
        return FAIL;
    }

    uint32_t name_len = wstrlen(entry_name, 256);
    uint32_t required_lfn_entry_count = (name_len + 12) / 13;

    uint32_t entry_start = fat32_find_continious_free_entries(inst, parent_entry.entry, required_lfn_entry_count + 1);
    if(entry_start == UINT32_MAX){
        udbP("FAT ERROR: Failed to create entry in parent directory!");
        return FAIL;
    }

    // open device
    udevice_storage_function_pointers* stor_device = _open_storage_device_(inst->storage_device);
    
    if(stor_device == nullptr){
        udbP("FAT ERROR: Failed to open storage device!");
        return FAIL;
    }

    fat32_cluster_entry* cluster_table = kmalloc(stor_device->get_lba_size()); // cluster entry table (or FAT)
    fat32_dir_entry* current_table = kmalloc(stor_device->get_lba_size() * inst->bpb.sector_per_cluster); // cluster
    fat32_long_dir_entry* long_current_table = (fat32_long_dir_entry*)current_table;

    uint64_t dir_entry_count_per_cluster = (stor_device->get_lba_size() * inst->bpb.sector_per_cluster) / sizeof(fat32_dir_entry);
    uint64_t cluster_entry_per_lba = stor_device->get_lba_size() / sizeof(fat32_cluster_entry);

    // write entries
    uint8_t sfn_chksum = fat32_calculate_lfn_chksum(sfn_name);
    size_t last_lfn_pos = 0;
    
    fat32_cluster_entry parent_dir_cluster_start = (parent_entry.entry.DIR_FstClusLO | (parent_entry.entry.DIR_FstClusHI << 16));
    fat32_cluster_chain chain = fat32_get_chain(inst, stor_device, parent_dir_cluster_start);

    if(chain.chain == nullptr){
        udbP("FAT ERROR: Failed to read cluster chain!");
        if(uobject_close_object(inst->storage_device) == FAIL){
            udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
        }
        kfree(current_table);
        kfree(cluster_table);
        return FAIL;
    }

    uint32_t current_read_relative_cluster = FAT32_CLUSTER_BAD_CLUSTER;
    for(size_t i = 0; i < required_lfn_entry_count + 1; i++){
        uint32_t current_relative_cluster = (entry_start + i) / dir_entry_count_per_cluster;
        uint32_t current_entry = (entry_start + i) % dir_entry_count_per_cluster;

        // read cluster if not already read cluster is matching
        if(current_read_relative_cluster != current_relative_cluster){
            if(current_read_relative_cluster != FAT32_CLUSTER_BAD_CLUSTER){
                // writeback current cluster if it is valid
                if(stor_device->write(inst->data_first_lba + ((chain.chain[current_read_relative_cluster] - 2) * inst->bpb.sector_per_cluster), inst->bpb.sector_per_cluster, (uint8_t*)(current_table)) == FAIL){
                    udbP("FAT ERROR: Failed to writeback directory cluster!");
                    if(uobject_close_object(inst->storage_device) == FAIL){
                        udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
                    }
                    kfree(cluster_table);
                    kfree(current_table);
                    kfree(chain.chain);
                    return FAIL;
                }
            }

            // read new cluster
            if(stor_device->read(inst->data_first_lba + ((chain.chain[current_relative_cluster] - 2) * inst->bpb.sector_per_cluster), inst->bpb.sector_per_cluster, (uint8_t*)(current_table)) == FAIL){
                udbP("FAT ERROR: Failed to read directory cluster!");
                if(uobject_close_object(inst->storage_device) == FAIL){
                    udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
                }
                kfree(cluster_table);
                kfree(current_table);
                kfree(chain.chain);
                return FAIL;
            }
            current_read_relative_cluster = current_relative_cluster;
        }

        if(i == 0){
            // first lfn entry
            long_current_table[current_entry].LDIR_Ord = required_lfn_entry_count | FAT32_LFN_ID_FLAG_BEGIN;
            long_current_table[current_entry].LDIR_FstClusLO = 0;
            long_current_table[current_entry].LDIR_Type = 0;
            long_current_table[current_entry].LDIR_Chksum = sfn_chksum;
            long_current_table[current_entry].LDIR_Attr = FAT32_ATTR_LONG_FILE_NAME;
            last_lfn_pos = put_in_lfn(&long_current_table[current_entry], entry_name, last_lfn_pos);
        }else if(i == required_lfn_entry_count){
            // last sfn entry
            current_table[current_entry].DIR_Attr = attrbs;
            current_table[current_entry].DIR_CrtDate = 0;
            current_table[current_entry].DIR_CrtTime = 0;
            current_table[current_entry].DIR_CrtTimeTenth = 0;
            current_table[current_entry].DIR_LstAccDate = 0;
            memcpy(current_table[current_entry].DIR_Name, sfn_name, 11);
            current_table[current_entry].DIR_NTRes = 0;
            current_table[current_entry].DIR_FileSize = file_size;
            current_table[current_entry].DIR_FstClusLO = assigned_cluster & 0xFFFF;
            current_table[current_entry].DIR_FstClusHI = assigned_cluster >> 16;
        }else{
            // middle lfn entries
            long_current_table[current_entry].LDIR_Ord = required_lfn_entry_count - i;
            long_current_table[current_entry].LDIR_FstClusLO = 0;
            long_current_table[current_entry].LDIR_Type = 0;
            long_current_table[current_entry].LDIR_Chksum = sfn_chksum;
            long_current_table[current_entry].LDIR_Attr = FAT32_ATTR_LONG_FILE_NAME;
            last_lfn_pos = put_in_lfn(&long_current_table[current_entry], entry_name, last_lfn_pos);
        }
    }

    // writeback last current cluster
    if(stor_device->write(inst->data_first_lba + ((chain.chain[current_read_relative_cluster] - 2) * inst->bpb.sector_per_cluster), inst->bpb.sector_per_cluster, (uint8_t*)(current_table)) == FAIL){
        udbP("FAT ERROR: Failed to writeback directory cluster!");
        if(uobject_close_object(inst->storage_device) == FAIL){
            udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
        }
        kfree(cluster_table);
        kfree(current_table);
        kfree(chain.chain);
        return FAIL;
    }

    if(uobject_close_object(inst->storage_device) == FAIL){
        udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
    }
    kfree(cluster_table);
    kfree(current_table);
    kfree(chain.chain);
    return SUCCESS;
}

uos_result fat32_create_file(u_fs_interface* interface, const wide_char_t* parent, const wide_char_t* file_name){
    if(fat32_check_name(file_name) != SUCCESS){
        udbP("FAT ERROR: Illegal name!");
        return FAIL;
    }
    
    fat32_instance* inst = fat32_find_instance(interface->partition);

    fat32_dir_entry root_dir = {};
    fat32_get_root_dir_virt_entry(inst, &root_dir);

    fat32_entry_result parent_entry = {};
    memcpy(&(parent_entry.entry), &root_dir, sizeof(fat32_dir_entry));

    uint64_t up_dir_count = w_parse_read_count(parent, '/');

    wide_char_t read_part[256];
    for(size_t i = 0; i < up_dir_count; i++){
        w_parse_read(read_part, parent, '/', i);

        if(read_part[0] == '\0'){
            continue; // empty skip
        }else{
            parent_entry = fat32_find_entry(inst, &(parent_entry.entry), read_part, "");
            if(parent_entry.entry.DIR_Name[0] == FAT32_DIR_ENTRY_FREE){
                udbP("FAT ERROR: Parent file not found!");
                return FAIL;
            }
        }
    }

    /*
    udevice_storage_function_pointers* stor_device = _open_storage_device_(inst->storage_device);
    fat32_cluster_entry dir_clus = FAT32_CLUSTER_FREE;
    if(fat32_allocate_cluster(inst, stor_device, FAT32_NEW_CLUSTER, &dir_clus, 1, true) != SUCCESS){
        udbP("FAT ERROR: Failed to create new file!");
        if(uobject_close_object(inst->storage_device) != SUCCESS){
            udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
        }
        return FAIL;
    }

    if(uobject_close_object(inst->storage_device) != SUCCESS){
        udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
        return FAIL;
    }
    */

    // no sfn name for auto random sfn name
    if(fat32_create_dir_entry(inst, parent_entry, file_name, "", 0, FAT32_CLUSTER_FREE, 0) != SUCCESS){
        udbP("FAT ERROR: Failed to create new file!");
        return FAIL;
    }

    return SUCCESS;
}

uos_result fat32_create_dir(u_fs_interface* interface, const wide_char_t* parent, const wide_char_t* dir_name){
    if(fat32_check_name(dir_name) != SUCCESS){
        udbP("FAT ERROR: Illegal name!");
        return FAIL;
    }
    
    udbPs();
    udbP_STR("creating a dir: ");
    udbP_wSTR(dir_name);
    udbP_STR(" in : ");
    udbP_wSTR(parent);
    udbPe();

    fat32_instance* inst = fat32_find_instance(interface->partition);

    fat32_dir_entry root_dir = {};
    fat32_get_root_dir_virt_entry(inst, &root_dir);

    fat32_entry_result parent_entry = {};
    memcpy(&(parent_entry.entry), &root_dir, sizeof(fat32_dir_entry));

    uint64_t up_dir_count = w_parse_read_count(parent, '/');

    wide_char_t read_part[256];
    for(size_t i = 0; i < up_dir_count; i++){
        w_parse_read(read_part, parent, '/', i);

        if(read_part[0] == '\0'){
            continue; // empty skip
        }else{
            parent_entry = fat32_find_entry(inst, &(parent_entry.entry), read_part, "");
            if(parent_entry.entry.DIR_Name[0] == FAT32_DIR_ENTRY_FREE){
                udbP("FAT ERROR: Parent directory not found!");
                return FAIL;
            }
        }
    }

    udevice_storage_function_pointers* stor_device = _open_storage_device_(inst->storage_device);
    fat32_cluster_entry dir_clus = FAT32_CLUSTER_FREE;
    if(fat32_allocate_cluster(inst, stor_device, FAT32_NEW_CLUSTER, &dir_clus, 1, true) != SUCCESS){
        udbP("FAT ERROR: Failed to create new directory!");
        if(uobject_close_object(inst->storage_device) != SUCCESS){
            udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
        }
        return FAIL;
    }

    // write . & .. entries
    fat32_dir_entry* dir_table = kmalloc(stor_device->get_lba_size() * inst->bpb.sector_per_cluster);
    memset(dir_table, 0x0, stor_device->get_lba_size() * inst->bpb.sector_per_cluster);
    
    // .
    dir_table[0].DIR_Name[0] = '.';
    for(size_t i = 1; i < 11; i++){
        dir_table[0].DIR_Name[i] = 0x20;
    }
    dir_table[0].DIR_Attr = FAT32_ATTR_DIRECTORY_BIT;
    dir_table[0].DIR_NTRes = 0;
    dir_table[0].DIR_CrtDate = 0;
    dir_table[0].DIR_CrtTime = 0;
    dir_table[0].DIR_CrtTimeTenth = 0;
    dir_table[0].DIR_WrtTime = 0;
    dir_table[0].DIR_WrtDate = 0;
    dir_table[0].DIR_LstAccDate = 0;
    dir_table[0].DIR_FstClusHI = (dir_clus >> 16) & 0xFFFF; // self
    dir_table[0].DIR_FstClusLO = dir_clus & 0xFFFF; // self
    dir_table[0].DIR_FileSize = 0;

    // ..
    dir_table[1].DIR_Name[0] = '.';
    dir_table[1].DIR_Name[1] = '.';
    for(size_t i = 2; i < 11; i++){
        dir_table[1].DIR_Name[i] = 0x20;
    }
    dir_table[1].DIR_Attr = FAT32_ATTR_DIRECTORY_BIT;
    dir_table[1].DIR_NTRes = 0;
    dir_table[1].DIR_CrtDate = 0;
    dir_table[1].DIR_CrtTime = 0;
    dir_table[1].DIR_CrtTimeTenth = 0;
    dir_table[1].DIR_WrtTime = 0;
    dir_table[1].DIR_WrtDate = 0;
    dir_table[1].DIR_LstAccDate = 0;
    if(((((uint32_t)parent_entry.entry.DIR_FstClusHI) << 16) | (uint32_t)(parent_entry.entry.DIR_FstClusLO)) == inst->bpb.BPB_RootClus){
        dir_table[1].DIR_FstClusHI = 0ULL; // root is 0 for ..
        dir_table[1].DIR_FstClusLO = 0ULL; // root is 0 for ..
    }else{
        dir_table[1].DIR_FstClusHI = parent_entry.entry.DIR_FstClusHI; // parent
        dir_table[1].DIR_FstClusLO = parent_entry.entry.DIR_FstClusLO; // parent
    }
    dir_table[1].DIR_FileSize = 0;

    if(stor_device->write(inst->data_first_lba + ((dir_clus - 2) * inst->bpb.sector_per_cluster), inst->bpb.sector_per_cluster, (uint8_t*)(dir_table)) == FAIL){
        udbP("FAT ERROR: Failed to write directory entries!");
        if(uobject_close_object(inst->storage_device) == FAIL){
            udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
        }
        kfree(dir_table);
        return FAIL;
    }
    kfree(dir_table);

    if(uobject_close_object(inst->storage_device) != SUCCESS){
        udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
        return FAIL;
    }

    // no sfn name for auto random sfn name
    if(fat32_create_dir_entry(inst, parent_entry, dir_name, "", FAT32_ATTR_DIRECTORY_BIT, dir_clus, 0) != SUCCESS){
        udbP("FAT ERROR: Failed to create new directory!");
        return FAIL;
    }

    return SUCCESS;
}

uos_result fat32_get_childs(u_fs_interface* interface, const wide_char_t* dir_path, wide_char_t*** child_names, size_t* names_buffer_size){
    *names_buffer_size = 0;

    fat32_instance* inst = fat32_find_instance(interface->partition);

    fat32_dir_entry root_dir = {};
    fat32_get_root_dir_virt_entry(inst, &root_dir);

    fat32_entry_result parent_entry = {};
    memcpy(&(parent_entry.entry), &root_dir, sizeof(fat32_dir_entry));

    uint64_t up_dir_count = w_parse_read_count(dir_path, '/');

    wide_char_t read_part[256];
    for(size_t i = 0; i < up_dir_count; i++){
        w_parse_read(read_part, dir_path, '/', i);

        if(read_part[0] == '\0'){
            continue; // empty skip
        }else{
            parent_entry = fat32_find_entry(inst, &(parent_entry.entry), read_part, "");
            if(parent_entry.entry.DIR_Name[0] == FAT32_DIR_ENTRY_FREE){
                udbP("FAT ERROR: Parent directory not found!");
                return FAIL;
            }
        }
    }

    fat32_dir_entry parent = {};
    memcpy(&parent, &(parent_entry.entry), sizeof(fat32_dir_entry));

    if(!(parent.DIR_Attr & FAT32_ATTR_DIRECTORY_BIT)){
        udbP("FAT ERROR: Parent is not a directory!");
        return FAIL;
    }

    // open device
    udevice_storage_function_pointers* stor_device = _open_storage_device_(inst->storage_device);

    if(stor_device == nullptr){
        udbP("FAT ERROR: Failed to open storage device!");
        return FAIL;
    }

    uint64_t lba_size = stor_device->get_lba_size();
    uint64_t cluster_entry_per_lba = lba_size / sizeof(fat32_cluster_entry);
    uint64_t dir_entry_count_per_cluster = (lba_size * inst->bpb.sector_per_cluster) / sizeof(fat32_dir_entry);

    fat32_cluster_entry* cluster_table = kmalloc(lba_size); // cluster entry table (or FAT)
    fat32_dir_entry* current_table = kmalloc(lba_size * inst->bpb.sector_per_cluster); // cluster
    fat32_long_dir_entry* long_current_table = (fat32_long_dir_entry*)current_table;
    wide_char_t* current_lfn_name = kmalloc(256 * sizeof(wide_char_t)); // + nullterm
    memset(current_lfn_name, 0x0, 256 *  sizeof(wide_char_t));

    fat32_cluster_entry current_cluster = (parent.DIR_FstClusLO | (parent.DIR_FstClusHI << 16));
    size_t long_name_len = 0;
    uint8_t current_lfn_chksum = 0;
    uint8_t expected_lfn_id = 0;

    // dir search
    while(true){
        uint32_t cluster_entry_offset_in_lba = current_cluster % cluster_entry_per_lba;

        // read cluster entry
        // calculate which lba it is in fat & read
        if(stor_device->read(inst->fat1_first_lba + (current_cluster / cluster_entry_per_lba), 1, (uint8_t*)(cluster_table)) == FAIL){
            udbP("FAT ERROR: Failed to read FAT header!");
            if(uobject_close_object(inst->storage_device) == FAIL){
                udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
            }
            kfree(cluster_table);
            kfree(current_table);
            kfree(current_lfn_name);
            return FAIL;
        }

        // cluster dir search
        // read cluster (directory entries)
        if(stor_device->read(inst->data_first_lba + ((current_cluster - 2) * inst->bpb.sector_per_cluster), inst->bpb.sector_per_cluster, (uint8_t*)(current_table)) == FAIL){
            udbP("FAT ERROR: Failed to read directory cluster!");
            if(uobject_close_object(inst->storage_device) == FAIL){
                udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
            }
            kfree(cluster_table);
            kfree(current_table);
            kfree(current_lfn_name);
            return FAIL;
        }
            
        for(uint32_t i = 0; i < dir_entry_count_per_cluster; i++){
            if(current_table[i].DIR_Name[0] == FAT32_DIR_ENTRY_FREE_START){
                // no any entries available further
                if(uobject_close_object(inst->storage_device) == FAIL){
                    udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
                }
                kfree(cluster_table);
                kfree(current_table);
                kfree(current_lfn_name);
                return SUCCESS;
            }else if(current_table[i].DIR_Name[0] != FAT32_DIR_ENTRY_FREE){
                // check long name
                if(current_table[i].DIR_Attr == FAT32_ATTR_LONG_FILE_NAME){
                    if(long_name_len){
                        if(long_current_table[i].LDIR_Ord == expected_lfn_id && long_current_table[i].LDIR_Chksum == current_lfn_chksum){
                            long_name_len = prepend_lfn(&long_current_table[i], current_lfn_name, long_name_len);
                            expected_lfn_id--;
                        }else{
                            // possiable fs corruption
                            udbP("FAT ERROR: LFN mismatch0 found! Maybe filesystem is corrupted?");
                            if(uobject_close_object(inst->storage_device) == FAIL){
                                udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
                            }
                            kfree(cluster_table);
                            kfree(current_table);
                            kfree(current_lfn_name);
                            return FAIL;
                        }
                    }else{
                        if(long_current_table[i].LDIR_Ord & FAT32_LFN_ID_FLAG_BEGIN){
                            long_name_len = prepend_lfn(&long_current_table[i], current_lfn_name, long_name_len);
                            current_lfn_chksum = long_current_table[i].LDIR_Chksum;
                            expected_lfn_id = (long_current_table[i].LDIR_Ord & ~FAT32_LFN_ID_FLAG_BEGIN) - 1;
                        }else{
                            // possiable fs corruption
                            udbP("FAT ERROR: LFN mismatch1 found! Maybe filesystem is corrupted?");
                            if(uobject_close_object(inst->storage_device) == FAIL){
                                udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
                            }
                            kfree(cluster_table);
                            kfree(current_table);
                            kfree(current_lfn_name);
                            return FAIL;
                        }
                    }
                }else{
                    if(long_name_len){
                        if(current_lfn_chksum == fat32_calculate_lfn_chksum(current_table[i].DIR_Name) && expected_lfn_id == 0){
                            char* current_name = kmalloc(12 * sizeof(char));
                            memcpy(current_name, current_table[i].DIR_Name, 11);
                            current_name[11] = '\0';

                            (*child_names) = kreloc((*child_names), *names_buffer_size * sizeof(wide_char_t*), ((*names_buffer_size) + 1) * sizeof(wide_char_t*));
                            (*names_buffer_size)++;
                            (*child_names)[(*names_buffer_size) - 1] = kmalloc((wstrlen(current_lfn_name, 257) + 1) * sizeof(wide_char_t));
                            memcpy(((*child_names)[(*names_buffer_size) - 1]), current_lfn_name, (wstrlen(current_lfn_name, 257) + 1) * sizeof(wide_char_t));
                            (*child_names)[(*names_buffer_size) - 1][wstrlen(current_lfn_name, 257)] = '\0';

                            //udbPs();
                            //udbP_wSTR((*child_names)[(*names_buffer_size) - 1]);
                            //udbPe();

                            // go to next entry
                            long_name_len = 0;
                            memset(current_lfn_name, 0x0, 256 *  sizeof(wide_char_t));
                            current_lfn_chksum = 0;
                            expected_lfn_id = 0;
                            kfree(current_name);
                            continue;

                        }else{
                            // possiable fs corruption
                            udbP("FAT ERROR: LFN mismatch2 found! Maybe filesystem is corrupted?");
                            if(uobject_close_object(inst->storage_device) == FAIL){
                                udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
                            }
                            kfree(cluster_table);
                            kfree(current_table);
                            kfree(current_lfn_name);
                            return FAIL;
                        }
                    }else{
                        char* current_name = kmalloc(12 * sizeof(char));
                        memcpy(current_name, current_table[i].DIR_Name, 11);
                        current_name[11] = '\0';

                        (*child_names) = kreloc((*child_names), *names_buffer_size * sizeof(wide_char_t*), ((*names_buffer_size) + 1) * sizeof(wide_char_t*));
                        (*names_buffer_size)++;
                        (*child_names)[(*names_buffer_size) - 1] = kmalloc((12) * sizeof(wide_char_t));
                        char_to_utf16_buf(current_name, ((*child_names)[(*names_buffer_size) - 1]));

                        //udbPs();
                        //udbP_wSTR((*child_names)[(*names_buffer_size) - 1]);
                        //udbPe();
                        
                        // go to next entry
                        kfree(current_name);
                        continue;
                    }
                }
            }
        }
        
        // not found go to next dir entry cluster
        if(FAT32_CHECK_IS_CLUSTER_ALLOCATED(cluster_table[cluster_entry_offset_in_lba])){
            current_cluster = cluster_table[cluster_entry_offset_in_lba];
        }else{
            break; // no any more cluster left
        }

    }

    if(uobject_close_object(inst->storage_device) == FAIL){
        udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
    }
    kfree(cluster_table);
    kfree(current_table);
    kfree(current_lfn_name);
    return SUCCESS;
}

uos_result mount_fat32_partition(partition_info partition, uobject_ref storage_device){
    udevice_emmc_storage_function_pointers* emmc =  _open_emmc_storage_device_(storage_device);

    if(emmc == nullptr){
        return FAIL;
    }

    fat32_instance inst;

    inst.storage_device = storage_device;
    inst.partition = partition;

    if(emmc->common.read(partition.start_lba + 0, 1, (uint8_t*)(&inst.bpb)) == FAIL){
        udbP("FAT ERROR: Failed to read FAT header!");
        if(uobject_close_object(storage_device) == FAIL){
            udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
        }
        return FAIL;
    }

    if(emmc->common.read(partition.start_lba + inst.bpb.BPB_FSInfo, 1, (uint8_t*)(&inst.FSInfo)) == FAIL){
        udbP("FAT ERROR: Failed to read FSInfo header!");
        if(uobject_close_object(storage_device) == FAIL){
            udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
        }
        return FAIL;
    }

    inst.fat1_first_lba = partition.start_lba + inst.bpb.reserved_sector_count;
    inst.fat2_first_lba = inst.fat1_first_lba + inst.bpb.BPB_FATSz32;
    inst.data_first_lba = inst.fat2_first_lba + inst.bpb.BPB_FATSz32;
    inst.data_cluster_count = (inst.bpb.TotSec32 - inst.bpb.reserved_sector_count - (inst.bpb.BPB_FATSz32 * inst.bpb.number_of_fats)) / inst.bpb.sector_per_cluster;

    fat32_create_new_instance(&inst);

    if(uobject_close_object(storage_device) == FAIL){
        udbP("FAT STORAGE DEVICE ERROR: Failed to close storage device!");
        return FAIL;
    }

    u_fs_interface fat32_fsi;
    memcpy(fat32_fsi.fs_type_name, "FAT32", 6);
    fat32_fsi.storage_device = storage_device;
    memcpy(&fat32_fsi.partition, &partition, sizeof(partition_info));

    // set functions
    fat32_fsi.create = fat32_create_file;
    fat32_fsi.create_dir = fat32_create_dir;
    fat32_fsi.delete = nullptr;
    fat32_fsi.read_file_info = nullptr;
    fat32_fsi.read = nullptr;
    fat32_fsi.write = nullptr;
    fat32_fsi.repair_fs = nullptr;
    fat32_fsi.rename = nullptr;
    fat32_fsi.get_childs = fat32_get_childs;

    uobject_ref obj = uobject_create_fsi(partition.name, 0, fat32_fsi);
    if(obj == UOBJECT_NOT_DEFINED){
        udbP("FAT UOBJECT ERROR: Failed to create filesystem interface UOBJECT!");
        return FAIL;
    }

    return vfs_create_object_file("/mounts/", partition.name, obj, VFS_TYPE_FS_PARTITION, 0);
}
