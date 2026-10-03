#include <u_kernel/filesystem/vfs/vfs.h>
#include <u_kernel/util/u_cstr_util.h>
#include <u_kernel/util/u_ctypes.h>

vfs_entry root;

void vfs_init(){
    strcpy(root.name, "root");
    root.dir_table = palloc(1); // allocate table
    memset(root.dir_table, 0x0, 4096);
    root.obj_ref = UOBJECT_NOT_DEFINED;
    root.flags = 0x0;
    root.type = VFS_TYPE_DIRECTORY;
}

// not filesystem endpoint
vfs_entry* vfs_find_child(vfs_entry* parent, const char* name){
    if(parent->type != VFS_TYPE_DIRECTORY){
        udbP("VFS ERROR: Parent is not a directory:");
        udbP(parent->name);
        return nullptr;
    }

    vfs_entry* looking_table = parent->dir_table;

    while (looking_table != nullptr)
    {
        for (size_t j = 0; j < VFS_ENTRY_COUNT_PER_PAGE - 1; j++)
        {

            if(looking_table[j].type != VFS_TYPE_EMPTY && !strcmp(name, looking_table[j].name)){
                // found return
                return &looking_table[j];
            }
        }

        // get next extension table if exsists
        if(looking_table[VFS_ENTRY_COUNT_PER_PAGE - 1].type == VFS_TYPE_DIRECTORY_EXTENSION){
            looking_table = looking_table[VFS_ENTRY_COUNT_PER_PAGE - 1].dir_table;
        }else{
            looking_table = nullptr;
        }
    }
    udbP("VFS ERROR: Child does not exists:");
    udbP(name);
    return nullptr;
}
vfs_entry* vfs_find_entry(const char* path){
    
    if (path == nullptr || path[0] == '\0') return nullptr;

    // "/" = root
    if (strcmp(path, "/") == 0) return &root;

    char current_name[256];
    vfs_entry* current = &root;

    // find
    size_t finding_entry_count = parse_read_count((char*)path, '/');
    for (size_t i = 0; i < finding_entry_count; i++)
    {
        parse_read(current_name, (char*)path, '/', i);
        if(strlen(current_name, 256) == 0) continue; // empty skip

        current = vfs_find_child(current, current_name);

        if(current == nullptr){
            return nullptr;
        }else{
            if(i == (finding_entry_count - 1)){
                return current;
            }
        }
        
    }

    udbP("VFS ERROR: Should not reach here:");
    udbP(path);
    return nullptr;
}

// filesystem endpoint
typedef struct{
    vfs_entry* fsi_file;
    wide_char_t* call_path; // do not forget to free this after use
}filesystem_call_info;

filesystem_call_info vfs_find_fsi_entry(const wide_char_t* path){
    filesystem_call_info result;
    result.fsi_file = nullptr;
    result.call_path = nullptr;
    if (path == nullptr || path[0] == '\0'){ 
        udbP("VFS WARNING: File is not a file system endpoint! H0")
        return result;
    }

    wide_char_t current_name[256];
    char current_name_utf8[256];
    vfs_entry* current = &root;

    // find
    size_t finding_entry_count = w_parse_read_count(path, '/');
    for (size_t i = 0; i < finding_entry_count; i++)
    {
        w_parse_read(current_name, path, '/', i);
        if(wstrlen(current_name, 256) == 0) continue; // empty skip
        utf16_to_char_buf(current_name, current_name_utf8);
        current = vfs_find_child(current, current_name_utf8);

        if(current == nullptr){
            udbP("VFS WARNING: File is not a file system endpoint! H1");
            return result;
        }else{
            if(current->type == VFS_TYPE_FS_PARTITION){
                result.fsi_file = current;
                result.call_path = kmalloc((wstrlen(path, 8196) + 1) * sizeof(wide_char_t));
                w_parse_read_multiple(result.call_path, path, '/', i + 1, UINT64_MAX); // read to buffer
                udbPw(result.call_path);
                return result;
            }
        }
        
    }

    udbP("VFS ERROR: FSI end pount not found:");
    udbPw(path);
    result.fsi_file = nullptr;
    result.call_path = nullptr;
    return result;
}

vfs_entry* vfs_add_entry(const char* dir_path, vfs_entry entry){
    vfs_entry* parent = vfs_find_entry(dir_path);

    if(parent == nullptr){
        udbP("VFS ERROR: Adding entry failed! Parent does not exsists.");
        return FAIL;
    }

    if(parent->type != VFS_TYPE_DIRECTORY){
        udbP("VFS ERROR: Adding entry failed! Parent is not a directory.");
        return FAIL;
    }

    // find empty slot
    vfs_entry* table = parent->dir_table;
    vfs_entry* found = nullptr;
    while (table != nullptr)
    {
        for(size_t i = 0; i < VFS_ENTRY_COUNT_PER_PAGE - 1; i++){
            if(table[i].type == VFS_TYPE_EMPTY){
                found = &table[i];
                break;
            }else{
                if(!strcmp(table[i].name, entry.name)){
                    udbP("VFS ERROR: Adding entry failed! Entry with same name already exsists.");
                    return nullptr;
                }
            }
        }

        // get next extension table if exsists
        if(table[VFS_ENTRY_COUNT_PER_PAGE - 1].type == VFS_TYPE_DIRECTORY_EXTENSION){
            table = table[VFS_ENTRY_COUNT_PER_PAGE - 1].dir_table;
        }else{
            // if found break
            if(found != nullptr){
                break;
            }else{
                // create new table
                table[VFS_ENTRY_COUNT_PER_PAGE - 1].type = VFS_TYPE_DIRECTORY_EXTENSION;
                table[VFS_ENTRY_COUNT_PER_PAGE - 1].dir_table = palloc(1); // allocate table
                memset(table[VFS_ENTRY_COUNT_PER_PAGE - 1].dir_table, 0x0, 4096);
                table = table[VFS_ENTRY_COUNT_PER_PAGE - 1].dir_table;
            }
        }
    }

    *found = entry;
    return found;
}

uos_result vfs_create_directory(const char* dir_path, const char* name){
    if(vfs_check_name(name) == FAIL){
        udbP("VFS ERROR: Illegal name!");
        return FAIL;
    }

    vfs_entry entry = {};
    entry.type = VFS_TYPE_DIRECTORY;
    strcpy(entry.name, name);
    entry.dir_table = palloc(1); // allocate table
    memset(entry.dir_table, 0x0, 4096);
    entry.obj_ref = UOBJECT_NOT_DEFINED;
    entry.flags = 0x0;

    if(vfs_add_entry(dir_path, entry) == nullptr){
        udbP("VFS ERROR: An error occurred while adding entry.");
        return FAIL;
    }else{
        return SUCCESS;
    }
}

uos_result vfs_create_device(const char* dir_path, const char* name, uobject_ref object){
    if(vfs_check_name(name) == FAIL){
        udbP("VFS ERROR: Illegal name!");
        return FAIL;
    }

    vfs_entry entry = {};
    entry.type = VFS_TYPE_DEVICE;
    strcpy(entry.name, name);
    entry.dir_table = nullptr;
    entry.obj_ref = object;
    entry.flags = 0x0;

    /*
    uart_print("dev name: ");
    uart_print(name);
    uart_print(" obj ref: ");
    uart_print_dec(object);
    uart_print("\n");
    */

    if(vfs_add_entry(dir_path, entry) == nullptr){
        udbP("VFS ERROR: An error occurred while adding entry.");
        return FAIL;
    }else{
        return SUCCESS;
    }
}

uos_result vfs_create_object_file(const char* dir_path, const char* name, uobject_ref object, uint32_t type, uint32_t flags){
    if(vfs_check_name(name) == FAIL){
        udbP("VFS ERROR: Illegal name!");
        return FAIL;
    }

    vfs_entry entry = {};
    entry.type = type;
    strcpy(entry.name, name);
    entry.dir_table = nullptr;
    entry.obj_ref = object;
    entry.flags = flags;

    if(vfs_add_entry(dir_path, entry) == nullptr){
        udbP("VFS ERROR: An error occurred while adding entry.");
        return FAIL;
    }else{
        return SUCCESS;
    }
}

uobject_ref vfs_get_device_ref(const char* path){
    vfs_entry* entry = vfs_find_entry(path);

    if(entry == nullptr){
        udbP("VFS ERROR: File does not exists!");
        return UOBJECT_NOT_DEFINED;
    }

    if(entry->type != VFS_TYPE_DEVICE){
        udbP("VFS ERROR: This is not a device!");
        return UOBJECT_NOT_DEFINED;
    }

    return entry->obj_ref;
}

uos_result vfs_check_name(const char* name){
    size_t name_len = strlen(name, 1024);
    if(name_len > 103 || name_len == 0){
        udbP("VFS ERROR: Empty or over 103 character name is not allowed!");
        return FAIL;
    }

    char last = '\0';
    for(size_t i = 0; i < name_len; i++){
        switch (name[i])
        {
        case '/':
            udbP("VFS ERROR: \"/\" in name is not allowed!");
            return FAIL;
        default:
            continue;
        }
    }
    return SUCCESS;
}

void _vfs_debug_list_under_dir_(const char* dir){
    vfs_entry* ent = vfs_find_entry(dir);
    if(ent == nullptr) {
        udbP("VFS DEBUG ERROR: Directory does not exists!");
        return;
    }

    udbPs();
    udbP_STR("dir: ");
    udbP_STR(dir);
    udbPe();

    vfs_entry* looking_table = ent->dir_table;
    while (looking_table != nullptr)
    {
        for (size_t j = 0; j < VFS_ENTRY_COUNT_PER_PAGE - 1; j++)
        {
            if(looking_table[j].type != VFS_TYPE_EMPTY){
                udbPs();
                udbP_STR("\"");
                udbP_STR(looking_table[j].name);
                udbP_STR("\" type: ");
                udbP_DEC(looking_table[j].type);
                udbPe();
            }
        }

        // get next extension table if exsists
        if(looking_table[VFS_ENTRY_COUNT_PER_PAGE - 1].type == VFS_TYPE_DIRECTORY_EXTENSION){
            looking_table = looking_table[VFS_ENTRY_COUNT_PER_PAGE - 1].dir_table;
        }else{
            looking_table = nullptr;
        }
    }
}

uos_result vfs_mount_gpt_partitions(uobject_ref storage_device_obj){

    return FAIL;
}

// filesystem endpoint functions
uos_result create_file(const wide_char_t* parent, const wide_char_t* file_name){
    filesystem_call_info call_info = vfs_find_fsi_entry(parent);
    if(call_info.call_path == nullptr || call_info.fsi_file == nullptr){
        udbP("VFS FSI ERROR: Filesystem not found!");
        if(call_info.call_path != nullptr){
            kfree(call_info.call_path);
        }
        return FAIL;
    }

    u_fs_interface* fs = _open_filesystem_interface_(call_info.fsi_file->obj_ref);
    if(fs == nullptr){
        udbP("VFS FSI ERROR: Failed to open filesystem interface!");
        kfree(call_info.call_path);
        return FAIL;
    }

    uos_result op_res = fs->create(fs, call_info.call_path, file_name);

    if(uobject_close_object(call_info.fsi_file->obj_ref) != SUCCESS){
        udbP("VFS FSI ERROR: Failed to close filesystem interface!");
        kfree(call_info.call_path);
        return FAIL;
    }

    kfree(call_info.call_path);
    return op_res;
}

uos_result create_directory(const wide_char_t* parent, const wide_char_t* dir_name){
    filesystem_call_info call_info = vfs_find_fsi_entry(parent);
    if(call_info.call_path == nullptr || call_info.fsi_file == nullptr){
        udbP("VFS FSI ERROR: Filesystem not found!");
        if(call_info.call_path == nullptr ){
            udbP("zorortortrotror");
        }
        if(call_info.call_path != nullptr){
            kfree(call_info.call_path);
        }
        return FAIL;
    }

    u_fs_interface* fs = _open_filesystem_interface_(call_info.fsi_file->obj_ref);
    if(fs == nullptr){
        udbP("VFS FSI ERROR: Failed to open filesystem interface!");
        kfree(call_info.call_path);
        return FAIL;
    }

    uos_result op_res = fs->create_dir(fs, call_info.call_path, dir_name);

    if(uobject_close_object(call_info.fsi_file->obj_ref) != SUCCESS){
        udbP("VFS FSI ERROR: Failed to close filesystem interface!");
        kfree(call_info.call_path);
        return FAIL;
    }
    
    kfree(call_info.call_path);
    return op_res;
}

uos_result delete_file(const wide_char_t* file_path){
    filesystem_call_info call_info = vfs_find_fsi_entry(file_path);
    if(call_info.call_path == nullptr || call_info.fsi_file == nullptr){
        udbP("VFS FSI ERROR: Filesystem not found!");
        if(call_info.call_path != nullptr){
            kfree(call_info.call_path);
        }
        return FAIL;
    }

    u_fs_interface* fs = _open_filesystem_interface_(call_info.fsi_file->obj_ref);
    if(fs == nullptr){
        udbP("VFS FSI ERROR: Failed to open filesystem interface!");
        kfree(call_info.call_path);
        return FAIL;
    }

    uos_result op_res = fs->delete(fs, call_info.call_path);

    if(uobject_close_object(call_info.fsi_file->obj_ref) != SUCCESS){
        udbP("VFS FSI ERROR: Failed to close filesystem interface!");
        kfree(call_info.call_path);
        return FAIL;
    }

    kfree(call_info.call_path);
    return op_res;
}

u_fs_file_info read_file_info(const wide_char_t* file_path){
    filesystem_call_info call_info = vfs_find_fsi_entry(file_path);
    u_fs_file_info op_res = {};
    if(call_info.call_path == nullptr || call_info.fsi_file == nullptr){
        udbP("VFS FSI ERROR: Filesystem not found!");
        if(call_info.call_path != nullptr){
            kfree(call_info.call_path);
        }
        return op_res;
    }

    u_fs_interface* fs = _open_filesystem_interface_(call_info.fsi_file->obj_ref);
    if(fs == nullptr){
        udbP("VFS FSI ERROR: Failed to open filesystem interface!");
        kfree(call_info.call_path);
        return op_res;
    }

    op_res = fs->read_file_info(fs, call_info.call_path);

    if(uobject_close_object(call_info.fsi_file->obj_ref) != SUCCESS){
        udbP("VFS FSI ERROR: Failed to close filesystem interface!");
        kfree(call_info.call_path);
        return op_res;
    }

    kfree(call_info.call_path);
    return op_res;
}

uos_result read(const wide_char_t* file_path, size_t read_offset, size_t read_len, void* read_buffer){
    filesystem_call_info call_info = vfs_find_fsi_entry(file_path);
    if(call_info.call_path == nullptr || call_info.fsi_file == nullptr){
        udbP("VFS FSI ERROR: Filesystem not found!");
        if(call_info.call_path != nullptr){
            kfree(call_info.call_path);
        }
        return FAIL;
    }

    u_fs_interface* fs = _open_filesystem_interface_(call_info.fsi_file->obj_ref);
    if(fs == nullptr){
        udbP("VFS FSI ERROR: Failed to open filesystem interface!");
        kfree(call_info.call_path);
        return FAIL;
    }

    uos_result op_res = fs->read(fs, call_info.call_path, read_offset, read_len, read_buffer);

    if(uobject_close_object(call_info.fsi_file->obj_ref) != SUCCESS){
        udbP("VFS FSI ERROR: Failed to close filesystem interface!");
        kfree(call_info.call_path);
        return FAIL;
    }

    kfree(call_info.call_path);
    return op_res;
}

uos_result write(const wide_char_t* file_path, size_t write_offset, size_t write_len, void* write_buffer){
    filesystem_call_info call_info = vfs_find_fsi_entry(file_path);
    if(call_info.call_path == nullptr || call_info.fsi_file == nullptr){
        udbP("VFS FSI ERROR: Filesystem not found!");
        if(call_info.call_path != nullptr){
            kfree(call_info.call_path);
        }
        return FAIL;
    }

    u_fs_interface* fs = _open_filesystem_interface_(call_info.fsi_file->obj_ref);
    if(fs == nullptr){
        udbP("VFS FSI ERROR: Failed to open filesystem interface!");
        kfree(call_info.call_path);
        return FAIL;
    }

    uos_result op_res = fs->write(fs, call_info.call_path, write_offset, write_len, write_buffer);

    if(uobject_close_object(call_info.fsi_file->obj_ref) != SUCCESS){
        udbP("VFS FSI ERROR: Failed to close filesystem interface!");
        kfree(call_info.call_path);
        return FAIL;
    }

    kfree(call_info.call_path);
    return op_res;
}

uos_result rename(const wide_char_t* file_path, const wide_char_t* new_name){
    filesystem_call_info call_info = vfs_find_fsi_entry(file_path);
    if(call_info.call_path == nullptr || call_info.fsi_file == nullptr){
        udbP("VFS FSI ERROR: Filesystem not found!");
        if(call_info.call_path != nullptr){
            kfree(call_info.call_path);
        }
        return FAIL;
    }

    u_fs_interface* fs = _open_filesystem_interface_(call_info.fsi_file->obj_ref);
    if(fs == nullptr){
        udbP("VFS FSI ERROR: Failed to open filesystem interface!");
        kfree(call_info.call_path);
        return FAIL;
    }

    uos_result op_res = fs->rename(fs, call_info.call_path, new_name);

    if(uobject_close_object(call_info.fsi_file->obj_ref) != SUCCESS){
        udbP("VFS FSI ERROR: Failed to close filesystem interface!");
        kfree(call_info.call_path);
        return FAIL;
    }

    kfree(call_info.call_path);
    return op_res;
}

uos_result get_dir_childs(const wide_char_t* dir_path, wide_char_t*** child_names, size_t* names_buffer_size){
    filesystem_call_info call_info = vfs_find_fsi_entry(dir_path);
    if(call_info.call_path == nullptr || call_info.fsi_file == nullptr){
        udbP("VFS FSI ERROR: Filesystem not found!");
        if(call_info.call_path != nullptr){
            kfree(call_info.call_path);
        }
        return FAIL;
    }

    u_fs_interface* fs = _open_filesystem_interface_(call_info.fsi_file->obj_ref);
    if(fs == nullptr){
        udbP("VFS FSI ERROR: Failed to open filesystem interface!");
        kfree(call_info.call_path);
        return FAIL;
    }

    uos_result op_res = fs->get_childs(fs, call_info.call_path, child_names, names_buffer_size);

    if(uobject_close_object(call_info.fsi_file->obj_ref) != SUCCESS){
        udbP("VFS FSI ERROR: Failed to close filesystem interface!");
        kfree(call_info.call_path);
        return FAIL;
    }

    kfree(call_info.call_path);
    return op_res;
}
