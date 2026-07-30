#ifndef SESSION_CONTACT_RESOLVER_H
#define SESSION_CONTACT_RESOLVER_H

#include <stdbool.h>
#include <stddef.h>

#define SESSION_CONTACT_ID_MAX 129
#define SESSION_CONTACT_LABEL_MAX 257

typedef struct {
    char device_id[SESSION_CONTACT_ID_MAX];
    char remark[SESSION_CONTACT_LABEL_MAX];
    char device_name[SESSION_CONTACT_LABEL_MAX];
    bool online;
} session_device_contact_t;

typedef enum {
    SESSION_CONTACT_MATCH_NONE = 0,
    SESSION_CONTACT_MATCH_DEVICE_ID,
    SESSION_CONTACT_MATCH_REMARK,
    SESSION_CONTACT_MATCH_DEVICE_NAME,
} session_contact_match_field_t;

typedef enum {
    SESSION_CONTACT_RESOLVE_INVALID = 0,
    SESSION_CONTACT_RESOLVE_NOT_FOUND,
    SESSION_CONTACT_RESOLVE_AMBIGUOUS,
    SESSION_CONTACT_RESOLVE_OFFLINE,
    SESSION_CONTACT_RESOLVE_FOUND,
} session_contact_resolve_status_t;

typedef struct {
    session_contact_resolve_status_t status;
    session_contact_match_field_t field;
    size_t index;
    size_t match_count;
    bool exact;
} session_contact_resolution_t;

session_contact_resolution_t session_contact_resolve(
    const session_device_contact_t *contacts,
    size_t count,
    const char *target);

#endif
