#include "../include/dbus.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <sys/random.h>

/* TODO: Make non-blocking when GPU Screen Recorder is turned into a library */
/* TODO: Make sure responses matches the requests */

#define DESKTOP_PORTAL_SIGNAL_RULE "type='signal',interface='org.freedesktop.Portal.Request'"

typedef enum {
    DICT_TYPE_STRING,
    DICT_TYPE_UINT32,
    DICT_TYPE_BOOL,
} dict_value_type;

typedef struct {
    const char *key;
    dict_value_type value_type;
    union {
        char *str;
        dbus_uint32_t u32;
        dbus_bool_t boolean;
    };
} dict_entry;

static const char* dict_value_type_to_string(dict_value_type type) {
    switch(type) {
        case DICT_TYPE_STRING: return "string";
        case DICT_TYPE_UINT32: return "uint32";
        case DICT_TYPE_BOOL:   return "boolean";
    }
    return "(unknown)";
}

static bool generate_random_characters(char *buffer, int buffer_size, const char *alphabet, size_t alphabet_size) {
    /* TODO: Use other functions on other platforms than linux */
    if(getrandom(buffer, buffer_size, 0) < buffer_size) {
        fprintf(stderr, "gsr error: generate_random_characters: failed to get random bytes, error: %s\n", strerror(errno));
        return false;
    }

    for(int i = 0; i < buffer_size; ++i) {
        unsigned char c = *(unsigned char*)&buffer[i];
        buffer[i] = alphabet[c % alphabet_size];
    }

    return true;
}

bool gsr_dbus_init(gsr_dbus *self, const char *screencast_restore_token) {
    memset(self, 0, sizeof(*self));
    dbus_error_init(&self->err);

    self->random_str[DBUS_RANDOM_STR_SIZE] = '\0';
    if(!generate_random_characters(self->random_str, DBUS_RANDOM_STR_SIZE, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 62)) {
        fprintf(stderr, "gsr error: gsr_dbus_init: failed to generate random string\n");
        return false;
    }

    self->con = dbus_bus_get(DBUS_BUS_SESSION, &self->err);
    if(dbus_error_is_set(&self->err)) {
        fprintf(stderr, "gsr error: gsr_dbus_init: dbus_bus_get failed with error: %s\n", self->err.message);
        return false;
    }

    if(!self->con) {
        fprintf(stderr, "gsr error: gsr_dbus_init: failed to get dbus session\n");
        return false;
    }

    /* TODO: Check the name */
    const int ret = dbus_bus_request_name(self->con, "com.dec05eba.gpu_screen_recorder", DBUS_NAME_FLAG_REPLACE_EXISTING, &self->err);
    if(dbus_error_is_set(&self->err)) {
        fprintf(stderr, "gsr error: gsr_dbus_init: dbus_bus_request_name failed with error: %s\n", self->err.message);
        gsr_dbus_deinit(self);
        return false;
    }

    if(screencast_restore_token) {
        self->screencast_restore_token = strdup(screencast_restore_token);
        if(!self->screencast_restore_token) {
            fprintf(stderr, "gsr error: gsr_dbus_init: failed to clone restore token\n");
            gsr_dbus_deinit(self);
            return false;
        }
    }

    (void)ret;
    // if(ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
    //     fprintf(stderr, "gsr error: gsr_capture_portal_setup_dbus: dbus_bus_request_name failed to get primary owner\n");
    //     return false;
    // }

    return true;
}

void gsr_dbus_deinit(gsr_dbus *self) {
    if(self->screencast_restore_token) {
        free(self->screencast_restore_token);
        self->screencast_restore_token = NULL;
    }

    if(self->desktop_portal_rule_added) {
        dbus_bus_remove_match(self->con, DESKTOP_PORTAL_SIGNAL_RULE, NULL);
        // dbus_connection_flush(self->con);
        self->desktop_portal_rule_added = false;
    }

    if(self->con) {
        dbus_error_free(&self->err);

        dbus_bus_release_name(self->con, "com.dec05eba.gpu_screen_recorder", NULL);

        // Apparently shouldn't be used when a connection is setup by using dbus_bus_get
        //dbus_connection_close(self->con);
        dbus_connection_unref(self->con);
        self->con = NULL;
    }
}

static bool gsr_dbus_desktop_portal_get_property(gsr_dbus *self, const char *interface, const char *property_name, uint32_t *result) {
    *result = 0;

    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.portal.Desktop",    // target for the method call
        "/org/freedesktop/portal/desktop",   // object to call on
        "org.freedesktop.DBus.Properties",   // interface to call on
        "Get");                              // method name
    if(!msg) {
        fprintf(stderr, "gsr error: gsr_dbus_desktop_portal_get_property: dbus_message_new_method_call failed\n");
        return false;
    }

    DBusMessageIter it;
    dbus_message_iter_init_append(msg, &it);

    if(!dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &interface)) {
        fprintf(stderr, "gsr error: gsr_dbus_desktop_portal_get_property: failed to add interface\n");
        dbus_message_unref(msg);
        return false;
    }

    if(!dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &property_name)) {
        fprintf(stderr, "gsr error: gsr_dbus_desktop_portal_get_property: failed to add property_name\n");
        dbus_message_unref(msg);
        return false;
    }

    DBusPendingCall *pending = NULL;
    if(!dbus_connection_send_with_reply(self->con, msg, &pending, -1) || !pending) { // -1 is default timeout
        fprintf(stderr, "gsr error: gsr_dbus_desktop_portal_get_property: dbus_connection_send_with_reply failed\n");
        dbus_message_unref(msg);
        return false;
    }
    dbus_connection_flush(self->con);

    //fprintf(stderr, "Request Sent\n");

    dbus_message_unref(msg);
    msg = NULL;

    dbus_pending_call_block(pending);

    msg = dbus_pending_call_steal_reply(pending);
    if(!msg) {
        fprintf(stderr, "gsr error: gsr_dbus_desktop_portal_get_property: dbus_pending_call_steal_reply failed\n");
        dbus_pending_call_unref(pending);
        dbus_message_unref(msg);
        return false;
    }

    dbus_pending_call_unref(pending);
    pending = NULL;

    DBusMessageIter resp_args;
    if(!dbus_message_iter_init(msg, &resp_args)) {
        fprintf(stderr, "gsr error: gsr_dbus_desktop_portal_get_property: response message is missing arguments\n");
        dbus_message_unref(msg);
        return false;
    } else if(DBUS_TYPE_UINT32 == dbus_message_iter_get_arg_type(&resp_args)) {
        dbus_message_iter_get_basic(&resp_args, result);
    } else if(DBUS_TYPE_VARIANT == dbus_message_iter_get_arg_type(&resp_args)) {
        DBusMessageIter variant_iter;
        dbus_message_iter_recurse(&resp_args, &variant_iter);

        if(dbus_message_iter_get_arg_type(&variant_iter) == DBUS_TYPE_UINT32) {
            dbus_message_iter_get_basic(&variant_iter, result);
        } else {
            fprintf(stderr, "gsr error: gsr_dbus_desktop_portal_get_property: response message is not a variant with an uint32, %c\n", dbus_message_iter_get_arg_type(&variant_iter));
            dbus_message_unref(msg);
            return false;
        }
    } else {
        fprintf(stderr, "gsr error: gsr_dbus_desktop_portal_get_property: response message is not an uint32, %c\n", dbus_message_iter_get_arg_type(&resp_args));
        dbus_message_unref(msg);
        return false;
        // TODO: Check dbus_error_is_set?
    }

    dbus_message_unref(msg);
    return true;
}

static uint32_t gsr_dbus_get_screencast_version_cached(gsr_dbus *self) {
    if(self->screencast_version == 0)
        gsr_dbus_desktop_portal_get_property(self, "org.freedesktop.portal.ScreenCast", "version", &self->screencast_version);
    return self->screencast_version;
}

static bool gsr_dbus_ensure_desktop_portal_rule_added(gsr_dbus *self) {
    if(self->desktop_portal_rule_added)
        return true;

    dbus_bus_add_match(self->con, DESKTOP_PORTAL_SIGNAL_RULE, &self->err);
    dbus_connection_flush(self->con);
    if(dbus_error_is_set(&self->err)) {
        fprintf(stderr, "gsr error: gsr_dbus_ensure_desktop_portal_rule_added: failed to add dbus rule %s, error: %s\n", DESKTOP_PORTAL_SIGNAL_RULE, self->err.message);
        return false;
    }
    self->desktop_portal_rule_added = true;
    return true;
}

static void gsr_dbus_portal_get_unique_handle_token(gsr_dbus *self, char *buffer, int size) {
    snprintf(buffer, size, "gpu_screen_recorder_handle_%s_%u", self->random_str, self->handle_counter++);
}

static void gsr_dbus_portal_get_unique_session_token(gsr_dbus *self, char *buffer, int size) {
    snprintf(buffer, size, "gpu_screen_recorder_session_%s", self->random_str);
}

static bool dbus_add_dict(DBusMessageIter *it, const dict_entry *entries, int num_entries) {
    DBusMessageIter array_it;
    if(!dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, "{sv}", &array_it))
        return false;

    for (int i = 0; i < num_entries; ++i) {
        DBusMessageIter entry_it = DBUS_MESSAGE_ITER_INIT_CLOSED;
        DBusMessageIter variant_it = DBUS_MESSAGE_ITER_INIT_CLOSED;

        if(!dbus_message_iter_open_container(&array_it, DBUS_TYPE_DICT_ENTRY, NULL, &entry_it))
            goto entry_err;

        if(!dbus_message_iter_append_basic(&entry_it, DBUS_TYPE_STRING, &entries[i].key))
            goto entry_err;

        switch (entries[i].value_type) {
            case DICT_TYPE_STRING: {
                if(!dbus_message_iter_open_container(&entry_it, DBUS_TYPE_VARIANT, DBUS_TYPE_STRING_AS_STRING, &variant_it))
                    goto entry_err;
                if(!dbus_message_iter_append_basic(&variant_it, DBUS_TYPE_STRING, &entries[i].str))
                    goto entry_err;
                break;
            }
            case DICT_TYPE_UINT32: {
                if(!dbus_message_iter_open_container(&entry_it, DBUS_TYPE_VARIANT, DBUS_TYPE_UINT32_AS_STRING, &variant_it))
                    goto entry_err;
                if(!dbus_message_iter_append_basic(&variant_it, DBUS_TYPE_UINT32, &entries[i].u32))
                    goto entry_err;
                break;
            }
            case DICT_TYPE_BOOL: {
                if(!dbus_message_iter_open_container(&entry_it, DBUS_TYPE_VARIANT, DBUS_TYPE_BOOLEAN_AS_STRING, &variant_it))
                    goto entry_err;
                if(!dbus_message_iter_append_basic(&variant_it, DBUS_TYPE_BOOLEAN, &entries[i].boolean))
                    goto entry_err;
                break;
            }
        }

        dbus_message_iter_close_container(&entry_it, &variant_it);
        dbus_message_iter_close_container(&array_it, &entry_it);
        continue;

        entry_err:
        dbus_message_iter_abandon_container_if_open(&array_it, &variant_it);
        dbus_message_iter_abandon_container_if_open(&array_it, &entry_it);
        dbus_message_iter_abandon_container_if_open(it, &array_it);
        return false;
    }

    return dbus_message_iter_close_container(it, &array_it);
}

/* If |response_msg| is NULL then we dont wait for a response signal */
static bool gsr_dbus_call_screencast_method(gsr_dbus *self, const char *method_name, const char *session_handle, const char *parent_window, const dict_entry *entries, int num_entries, int *resp_fd, DBusMessage **response_msg) {
    if(resp_fd)
        *resp_fd = -1;

    if(response_msg)
        *response_msg = NULL;

    if(!gsr_dbus_ensure_desktop_portal_rule_added(self))
        return false;

    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.portal.Desktop",    // target for the method call
        "/org/freedesktop/portal/desktop",   // object to call on
        "org.freedesktop.portal.ScreenCast", // interface to call on
        method_name);                        // method name
    if(!msg) {
        fprintf(stderr, "gsr error: gsr_dbus_call_screencast_method: dbus_message_new_method_call failed\n");
        return false;
    }

    DBusMessageIter it;
    dbus_message_iter_init_append(msg, &it);

    if(session_handle) {
        if(!dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &session_handle)) {
            fprintf(stderr, "gsr error: gsr_dbus_call_screencast_method: failed to add session_handle\n");
            dbus_message_unref(msg);
            return false;
        }
    }

    if(parent_window) {
        if(!dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &parent_window)) {
            fprintf(stderr, "gsr error: gsr_dbus_call_screencast_method: failed to add parent_window\n");
            dbus_message_unref(msg);
            return false;
        }
    }

    if(!dbus_add_dict(&it, entries, num_entries)) {
        fprintf(stderr, "gsr error: gsr_dbus_call_screencast_method: failed to add dict\n");
        dbus_message_unref(msg);
        return false;
    }

    DBusPendingCall *pending = NULL;
    if(!dbus_connection_send_with_reply(self->con, msg, &pending, -1) || !pending) { // -1 is default timeout
        fprintf(stderr, "gsr error: gsr_dbus_call_screencast_method: dbus_connection_send_with_reply failed\n");
        dbus_message_unref(msg);
        return false;
    }
    dbus_connection_flush(self->con);

    //fprintf(stderr, "Request Sent\n");

    dbus_message_unref(msg);
    msg = NULL;

    dbus_pending_call_block(pending);

    msg = dbus_pending_call_steal_reply(pending);
    if(!msg) {
        fprintf(stderr, "gsr error: gsr_dbus_call_screencast_method: dbus_pending_call_steal_reply failed\n");
        dbus_pending_call_unref(pending);
        dbus_message_unref(msg);
        return false;
    }

    dbus_pending_call_unref(pending);
    pending = NULL;

    DBusMessageIter resp_args;
    if(!dbus_message_iter_init(msg, &resp_args)) {
        fprintf(stderr, "gsr error: gsr_dbus_call_screencast_method: response message is missing arguments\n");
        dbus_message_unref(msg);
        return false;
    } else if (DBUS_TYPE_OBJECT_PATH == dbus_message_iter_get_arg_type(&resp_args)) {
        const char *res = NULL;
        dbus_message_iter_get_basic(&resp_args, &res);
    } else if(DBUS_TYPE_UNIX_FD == dbus_message_iter_get_arg_type(&resp_args)) {
        int fd = -1;
        dbus_message_iter_get_basic(&resp_args, &fd);

        if(resp_fd)
            *resp_fd = fd;
    } else if(DBUS_TYPE_STRING == dbus_message_iter_get_arg_type(&resp_args)) {
        char *err = NULL;
        dbus_message_iter_get_basic(&resp_args, &err);
        fprintf(stderr, "gsr error: gsr_dbus_call_screencast_method: failed with error: %s\n", err);

        dbus_message_unref(msg);
        return false;
        // TODO: Check dbus_error_is_set?
    } else {
        fprintf(stderr, "gsr error: gsr_dbus_call_screencast_method: response message is not an object path or unix fd\n");
        dbus_message_unref(msg);
        return false;
        // TODO: Check dbus_error_is_set?
    }

    dbus_message_unref(msg);
    if(!response_msg)
        return true;

    /* TODO: Add timeout, but take into consideration user interactive signals (such as selecting a monitor to capture for ScreenCast) */
    for (;;) {
        const int timeout_milliseconds = 10;
        dbus_connection_read_write(self->con, timeout_milliseconds);
        *response_msg = dbus_connection_pop_message(self->con);

        if(!*response_msg)
            continue;

        if(!dbus_message_is_signal(*response_msg, "org.freedesktop.portal.Request", "Response")) {
            dbus_message_unref(*response_msg);
            *response_msg = NULL;
            continue;
        }

        break;
    }

    return true;
}

static int gsr_dbus_get_response_status(DBusMessageIter *resp_args) {
    if(dbus_message_iter_get_arg_type(resp_args) != DBUS_TYPE_UINT32) {
        fprintf(stderr, "gsr error: gsr_dbus_get_response_status: missing uint32 in response\n");
        return -1;
    }

    dbus_uint32_t response_status = 0;
    dbus_message_iter_get_basic(resp_args, &response_status);

    dbus_message_iter_next(resp_args);
    return (int)response_status;
}

static dict_entry* find_dict_entry_by_key(dict_entry *entries, int num_entries, const char *key) {
    for(int i = 0; i < num_entries; ++i) {
        if(strcmp(entries[i].key, key) == 0)
            return &entries[i];
    }
    return NULL;
}

static bool gsr_dbus_get_variant_value(DBusMessageIter *iter, dict_entry *entry) {
    if(dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_VARIANT) {
        fprintf(stderr, "gsr error: gsr_dbus_get_variant_value: value is not a variant\n");
        return false;
    }

    DBusMessageIter variant_iter;
    dbus_message_iter_recurse(iter, &variant_iter);

    switch(dbus_message_iter_get_arg_type(&variant_iter)) {
        case DBUS_TYPE_STRING: {
            if(entry->value_type != DICT_TYPE_STRING) {
                fprintf(stderr, "gsr error: gsr_dbus_get_variant_value: expected entry value to be a(n) %s was a string\n", dict_value_type_to_string(entry->value_type));
                return false;
            }

            const char *value = NULL;
            dbus_message_iter_get_basic(&variant_iter, &value);

            if(!value) {
                fprintf(stderr, "gsr error: gsr_dbus_get_variant_value: failed to get entry value as value\n");
                return false;
            }

            if(entry->str) {
                free(entry->str);
                entry->str = NULL;
            }

            entry->str = strdup(value);
            if(!entry->str) {
                fprintf(stderr, "gsr error: gsr_dbus_get_variant_value: failed to copy value\n");
                return false;
            }
            return true;
        }
        case DBUS_TYPE_UINT32: {
            if(entry->value_type != DICT_TYPE_UINT32) {
                fprintf(stderr, "gsr error: gsr_dbus_get_variant_value: expected entry value to be a(n) %s was an uint32\n", dict_value_type_to_string(entry->value_type));
                return false;
            }

            dbus_message_iter_get_basic(&variant_iter, &entry->u32);
            return true;
        }
        case DBUS_TYPE_BOOLEAN: {
            if(entry->value_type != DICT_TYPE_BOOL) {
                fprintf(stderr, "gsr error: gsr_dbus_get_variant_value: expected entry value to be a(n) %s was a boolean\n", dict_value_type_to_string(entry->value_type));
                return false;
            }

            dbus_message_iter_get_basic(&variant_iter, &entry->boolean);
            return true;
        }
    }

    fprintf(stderr, "gsr error: gsr_dbus_get_variant_value: got unexpected type, expected string, uint32 or boolean\n");
    return false;
}

/*
    Parses a{sv} into matching key entries in |entries|.
    If the entry value is a string then it's allocated with malloc and is null-terminated
    and has to be free by the caller.
    The entry values should be 0 before this method is called.
    The entries are free'd if this function fails.
*/
static bool gsr_dbus_get_map(DBusMessageIter *resp_args, dict_entry *entries, int num_entries) {
    if(dbus_message_iter_get_arg_type(resp_args) != DBUS_TYPE_ARRAY) {
        fprintf(stderr, "gsr error: gsr_dbus_get_map: missing array in response\n");
        return false;
    }

    DBusMessageIter subiter;
    dbus_message_iter_recurse(resp_args, &subiter);

    while(dbus_message_iter_get_arg_type(&subiter) != DBUS_TYPE_INVALID) {
        DBusMessageIter dictiter = DBUS_MESSAGE_ITER_INIT_CLOSED;
        const char *key = NULL;
        dict_entry *entry = NULL;

        // fprintf(stderr, "    array element type: %c, %s\n",
        //         dbus_message_iter_get_arg_type(&subiter),
        //         dbus_message_iter_get_signature(&subiter));
        if(dbus_message_iter_get_arg_type(&subiter) != DBUS_TYPE_DICT_ENTRY) {
            fprintf(stderr, "gsr error: gsr_dbus_get_map: array value is not an entry\n");
            return false;
        }

        dbus_message_iter_recurse(&subiter, &dictiter);

        if(dbus_message_iter_get_arg_type(&dictiter) != DBUS_TYPE_STRING) {
            fprintf(stderr, "gsr error: gsr_dbus_get_map: entry key is not a string\n");
            goto error;
        }

        dbus_message_iter_get_basic(&dictiter, &key);
        if(!key) {
            fprintf(stderr, "gsr error: gsr_dbus_get_map: failed to get entry key as value\n");
            goto error;
        }
        
        entry = find_dict_entry_by_key(entries, num_entries, key);
        if(!entry) {
            dbus_message_iter_next(&subiter);
            continue;
        }

        if(!dbus_message_iter_next(&dictiter)) {
            fprintf(stderr, "gsr error: gsr_dbus_get_map: missing entry value\n");
            goto error;
        }

        if(!gsr_dbus_get_variant_value(&dictiter, entry))
            goto error;

        dbus_message_iter_next(&subiter);
    }

    return true;

    error:
    for(int i = 0; i < num_entries; ++i) {
        if(entries[i].value_type == DICT_TYPE_STRING) {
            free(entries[i].str);
            entries[i].str = NULL;
        }
    }
    return false;
}

int gsr_dbus_screencast_create_session(gsr_dbus *self, char **session_handle) {
    assert(session_handle);
    *session_handle = NULL;

    char handle_token[64];
    gsr_dbus_portal_get_unique_handle_token(self, handle_token, sizeof(handle_token));

    char session_handle_token[64];
    gsr_dbus_portal_get_unique_session_token(self, session_handle_token, sizeof(session_handle_token));

    dict_entry args[2];
    args[0].key = "handle_token";
    args[0].value_type = DICT_TYPE_STRING;
    args[0].str = handle_token;

    args[1].key = "session_handle_token";
    args[1].value_type = DICT_TYPE_STRING;
    args[1].str = session_handle_token;

    DBusMessage *response_msg = NULL;
    if(!gsr_dbus_call_screencast_method(self, "CreateSession", NULL, NULL, args, 2, NULL, &response_msg)) {
        fprintf(stderr, "gsr error: gsr_dbus_screencast_create_session: failed to setup ScreenCast session. Make sure you have a desktop portal running with support for the ScreenCast interface and that the desktop portal matches the Wayland compositor you are running.\n");
        return -1;
    }

    // TODO: Verify signal path matches |res|, maybe check the below
    // DBUS_TYPE_ARRAY value?
    //fprintf(stderr, "signature: %s, sender: %s\n", dbus_message_get_signature(msg), dbus_message_get_sender(msg));
    DBusMessageIter resp_args;
    if(!dbus_message_iter_init(response_msg, &resp_args)) {
        fprintf(stderr, "gsr error: gsr_dbus_screencast_create_session: missing response\n");
        dbus_message_unref(response_msg);
        return -1;
    }

    const int response_status = gsr_dbus_get_response_status(&resp_args);
    if(response_status != 0) {
        dbus_message_unref(response_msg);
        return response_status;
    }

    dict_entry entries[1];
    entries[0].key = "session_handle";
    entries[0].str = NULL;
    entries[0].value_type = DICT_TYPE_STRING;
    if(!gsr_dbus_get_map(&resp_args, entries, 1)) {
        dbus_message_unref(response_msg);
        return -1;
    }

    if(!entries[0].str) {
        fprintf(stderr, "gsr error: gsr_dbus_screencast_create_session: missing \"session_handle\" in response\n");
        dbus_message_unref(response_msg);
        return -1;
    }

    *session_handle = entries[0].str;
    //fprintf(stderr, "session handle: |%s|\n", entries[0].str);
    //free(entries[0].str);

    dbus_message_unref(response_msg);
    return 0;
}

int gsr_dbus_screencast_select_sources(gsr_dbus *self, const char *session_handle, gsr_portal_capture_type capture_type, gsr_portal_cursor_mode cursor_mode) {
    assert(session_handle);

    char handle_token[64];
    gsr_dbus_portal_get_unique_handle_token(self, handle_token, sizeof(handle_token));

    int num_arg_dict = 4;
    dict_entry args[6];
    args[0].key = "types";
    args[0].value_type = DICT_TYPE_UINT32;
    args[0].u32 = capture_type;

    args[1].key = "multiple";
    args[1].value_type = DICT_TYPE_BOOL;
    args[1].boolean = false; /* TODO: Wayland ignores this and still gives the option to select multiple sources. Support that case.. */

    args[2].key = "handle_token";
    args[2].value_type = DICT_TYPE_STRING;
    args[2].str = handle_token;

    args[3].key = "cursor_mode";
    args[3].value_type = DICT_TYPE_UINT32;
    args[3].u32 = cursor_mode;

    const int screencast_server_version = gsr_dbus_get_screencast_version_cached(self);
    if(screencast_server_version >= 4) {
        num_arg_dict = 5;
        args[4].key = "persist_mode";
        args[4].value_type = DICT_TYPE_UINT32;
        args[4].u32 = 2; /* persist until explicitly revoked */

        if(self->screencast_restore_token && self->screencast_restore_token[0]) {
            num_arg_dict = 6;

            args[5].key = "restore_token";
            args[5].value_type = DICT_TYPE_STRING;
            args[5].str = self->screencast_restore_token;
        }
    } else if(self->screencast_restore_token && self->screencast_restore_token[0]) {
        fprintf(stderr, "gsr warning: gsr_dbus_screencast_select_sources: tried to use restore token but this option is only available in screencast version >= 4, your wayland compositors screencast version is %d\n", screencast_server_version);
    }
    
    DBusMessage *response_msg = NULL;
    if(!gsr_dbus_call_screencast_method(self, "SelectSources", session_handle, NULL, args, num_arg_dict, NULL, &response_msg)) {
        if(num_arg_dict == 6) {
            /* We dont know what the error exactly is but assume it may be because of invalid restore token. In that case try without restore token */
            fprintf(stderr, "gsr warning: gsr_dbus_screencast_select_sources: SelectSources failed, retrying without restore_token\n");
            num_arg_dict = 5;
            if(!gsr_dbus_call_screencast_method(self, "SelectSources", session_handle, NULL, args, num_arg_dict, NULL, &response_msg))
                return -1;
        } else {
            return -1;
        }
    }

    // TODO: Verify signal path matches |res|, maybe check the below
    //fprintf(stderr, "signature: %s, sender: %s\n", dbus_message_get_signature(msg), dbus_message_get_sender(msg));
    DBusMessageIter resp_args;
    if(!dbus_message_iter_init(response_msg, &resp_args)) {
        fprintf(stderr, "gsr error: gsr_dbus_screencast_create_session: missing response\n");
        dbus_message_unref(response_msg);
        return -1;
    }

    
    const int response_status = gsr_dbus_get_response_status(&resp_args);
    if(response_status != 0) {
        dbus_message_unref(response_msg);
        return response_status;
    }

    dbus_message_unref(response_msg);
    return 0;
}

static dbus_uint32_t screencast_stream_get_pipewire_node(DBusMessageIter *iter) {
    DBusMessageIter subiter;
    dbus_message_iter_recurse(iter, &subiter);

    if(dbus_message_iter_get_arg_type(&subiter) == DBUS_TYPE_STRUCT) {
        DBusMessageIter structiter;
        dbus_message_iter_recurse(&subiter, &structiter);

        if(dbus_message_iter_get_arg_type(&structiter) == DBUS_TYPE_UINT32) {
            dbus_uint32_t data = 0;
            dbus_message_iter_get_basic(&structiter, &data);
            return data;
        }
    }

    return 0;
}

int gsr_dbus_screencast_start(gsr_dbus *self, const char *session_handle, uint32_t *pipewire_node) {
    assert(session_handle);
    *pipewire_node = 0;

    char handle_token[64];
    gsr_dbus_portal_get_unique_handle_token(self, handle_token, sizeof(handle_token));

    dict_entry args[1];
    args[0].key = "handle_token";
    args[0].value_type = DICT_TYPE_STRING;
    args[0].str = handle_token;
    
    DBusMessage *response_msg = NULL;
    if(!gsr_dbus_call_screencast_method(self, "Start", session_handle, "", args, 1, NULL, &response_msg))
        return -1;

    // TODO: Verify signal path matches |res|, maybe check the below
    //fprintf(stderr, "signature: %s, sender: %s\n", dbus_message_get_signature(msg), dbus_message_get_sender(msg));
    DBusMessageIter resp_args;
    if(!dbus_message_iter_init(response_msg, &resp_args)) {
        fprintf(stderr, "gsr error: gsr_dbus_screencast_start: missing response\n");
        dbus_message_unref(response_msg);
        return -1;
    }

    const int response_status = gsr_dbus_get_response_status(&resp_args);
    if(response_status != 0) {
        dbus_message_unref(response_msg);
        return response_status;
    }

    if(dbus_message_iter_get_arg_type(&resp_args) != DBUS_TYPE_ARRAY) {
        fprintf(stderr, "gsr error: gsr_dbus_screencast_start: missing array in response\n");
        dbus_message_unref(response_msg);
        return -1;
    }

    DBusMessageIter subiter;
    dbus_message_iter_recurse(&resp_args, &subiter);

    while(dbus_message_iter_get_arg_type(&subiter) != DBUS_TYPE_INVALID) {
        DBusMessageIter dictiter = DBUS_MESSAGE_ITER_INIT_CLOSED;
        const char *key = NULL;

        // fprintf(stderr, "    array element type: %c, %s\n",
        //         dbus_message_iter_get_arg_type(&subiter),
        //         dbus_message_iter_get_signature(&subiter));
        if(dbus_message_iter_get_arg_type(&subiter) != DBUS_TYPE_DICT_ENTRY) {
            fprintf(stderr, "gsr error: gsr_dbus_screencast_start: array value is not an entry\n");
            goto error;
        }

        dbus_message_iter_recurse(&subiter, &dictiter);

        if(dbus_message_iter_get_arg_type(&dictiter) != DBUS_TYPE_STRING) {
            fprintf(stderr, "gsr error: gsr_dbus_screencast_start: entry key is not a string\n");
            goto error;
        }

        dbus_message_iter_get_basic(&dictiter, &key);
        if(!key) {
            fprintf(stderr, "gsr error: gsr_dbus_screencast_start: failed to get entry key as value\n");
            goto error;
        }

        if(strcmp(key, "restore_token") == 0) {
            if(!dbus_message_iter_next(&dictiter)) {
                fprintf(stderr, "gsr error: gsr_dbus_screencast_start: missing restore_token value\n");
                goto error;
            }

            if(dbus_message_iter_get_arg_type(&dictiter) != DBUS_TYPE_VARIANT) {
                fprintf(stderr, "gsr error: gsr_dbus_screencast_start: restore_token is not a variant\n");
                goto error;
            }

            DBusMessageIter variant_iter;
            dbus_message_iter_recurse(&dictiter, &variant_iter);

            if(dbus_message_iter_get_arg_type(&variant_iter) != DBUS_TYPE_STRING) {
                fprintf(stderr, "gsr error: gsr_dbus_screencast_start: restore_token is not a string\n");
                goto error;
            }

            char *restore_token_str = NULL;
            dbus_message_iter_get_basic(&variant_iter, &restore_token_str);

            if(restore_token_str) {
                if(self->screencast_restore_token) {
                    free(self->screencast_restore_token);
                    self->screencast_restore_token = NULL;
                }
                self->screencast_restore_token = strdup(restore_token_str);
                //fprintf(stderr, "got restore token: %s\n", self->screencast_restore_token);
            }
        } else if(strcmp(key, "streams") == 0) {
            if(!dbus_message_iter_next(&dictiter)) {
                fprintf(stderr, "gsr error: gsr_dbus_screencast_start: missing streams value\n");
                goto error;
            }

            if(dbus_message_iter_get_arg_type(&dictiter) != DBUS_TYPE_VARIANT) {
                fprintf(stderr, "gsr error: gsr_dbus_screencast_start: streams value is not a variant\n");
                goto error;
            }

            DBusMessageIter variant_iter;
            dbus_message_iter_recurse(&dictiter, &variant_iter);

            if(dbus_message_iter_get_arg_type(&variant_iter) != DBUS_TYPE_ARRAY) {
                fprintf(stderr, "gsr error: gsr_dbus_screencast_start: streams value is not an array\n");
                goto error;
            }

            int num_streams = dbus_message_iter_get_element_count(&variant_iter);
            //fprintf(stderr, "num streams: %d\n", num_streams);
            /* Skip over all streams except the last one, since kde can return multiple streams even if only 1 is requested. The last one is the valid one */
            for(int i = 0; i < num_streams - 1; ++i) {
                screencast_stream_get_pipewire_node(&variant_iter);
            }

            if(num_streams > 0) {
                *pipewire_node = screencast_stream_get_pipewire_node(&variant_iter);
                //fprintf(stderr, "pipewire node: %u\n", *pipewire_node);
            }
        }

        dbus_message_iter_next(&subiter);
    }

    if(*pipewire_node == 0) {
        fprintf(stderr, "gsr error: gsr_dbus_screencast_start: no pipewire node returned\n");
        goto error;
    }

    dbus_message_unref(response_msg);
    return 0;

    error:
    dbus_message_unref(response_msg);
    return -1;
}

bool gsr_dbus_screencast_open_pipewire_remote(gsr_dbus *self, const char *session_handle, int *pipewire_fd) {
    assert(session_handle);
    *pipewire_fd = -1;
    return gsr_dbus_call_screencast_method(self, "OpenPipeWireRemote", session_handle, NULL, NULL, 0, pipewire_fd, NULL);
}

const char* gsr_dbus_screencast_get_restore_token(gsr_dbus *self) {
    return self->screencast_restore_token;
}
