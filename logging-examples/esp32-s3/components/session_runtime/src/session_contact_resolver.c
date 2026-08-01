#include "session_contact_resolver.h"

#include <stdint.h>
#include <string.h>

typedef struct {
    const char *data;
    size_t length;
} string_slice_t;

static unsigned char ascii_fold(unsigned char value)
{
    return value >= 'A' && value <= 'Z'
               ? (unsigned char)(value + ('a' - 'A'))
               : value;
}

static bool ascii_space(unsigned char value)
{
    return value == ' ' || value == '\t' || value == '\r' ||
           value == '\n' || value == '\f' || value == '\v';
}

static string_slice_t trim_target(const char *target)
{
    string_slice_t result = {
        .data = target,
        .length = target == NULL ? 0U : strlen(target),
    };
    while (result.length > 0U &&
           ascii_space((unsigned char)result.data[0])) {
        result.data++;
        result.length--;
    }
    while (result.length > 0U &&
           ascii_space((unsigned char)result.data[result.length - 1U])) {
        result.length--;
    }
    return result;
}

static bool equals_slice(const char *value, string_slice_t target)
{
    if (value == NULL || strlen(value) != target.length) {
        return false;
    }
    for (size_t index = 0U; index < target.length; ++index) {
        if (ascii_fold((unsigned char)value[index]) !=
            ascii_fold((unsigned char)target.data[index])) {
            return false;
        }
    }
    return true;
}

static bool contains_slice(const char *value, string_slice_t target)
{
    if (value == NULL || value[0] == '\0' || target.length == 0U) {
        return false;
    }
    size_t value_length = strlen(value);
    if (target.length > value_length) {
        return false;
    }
    for (size_t offset = 0U;
         offset + target.length <= value_length;
         ++offset) {
        bool matches = true;
        for (size_t index = 0U; index < target.length; ++index) {
            if (ascii_fold((unsigned char)value[offset + index]) !=
                ascii_fold((unsigned char)target.data[index])) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return true;
        }
    }
    return false;
}

static session_contact_resolution_t finish_unique(
    const session_device_contact_t *contacts,
    size_t index,
    session_contact_match_field_t field,
    bool exact)
{
    return (session_contact_resolution_t) {
        .status = contacts[index].online
                      ? SESSION_CONTACT_RESOLVE_FOUND
                      : SESSION_CONTACT_RESOLVE_OFFLINE,
        .field = field,
        .index = index,
        .match_count = 1U,
        .exact = exact,
    };
}

static session_contact_resolution_t resolve_labels(
    const session_device_contact_t *contacts,
    size_t count,
    string_slice_t target,
    bool exact)
{
    size_t matched_index = SIZE_MAX;
    size_t matches = 0U;
    session_contact_match_field_t matched_field = SESSION_CONTACT_MATCH_NONE;

    for (size_t index = 0U; index < count; ++index) {
        bool remark_matches =
            exact ? equals_slice(contacts[index].remark, target)
                  : contains_slice(contacts[index].remark, target);
        bool name_matches =
            exact ? equals_slice(contacts[index].device_name, target)
                  : contains_slice(contacts[index].device_name, target);
        if (!remark_matches && !name_matches) {
            continue;
        }
        matched_index = index;
        matched_field = remark_matches ? SESSION_CONTACT_MATCH_REMARK
                                       : SESSION_CONTACT_MATCH_DEVICE_NAME;
        matches++;
    }

    if (matches == 0U) {
        return (session_contact_resolution_t) {
            .status = SESSION_CONTACT_RESOLVE_NOT_FOUND,
        };
    }
    if (matches > 1U) {
        return (session_contact_resolution_t) {
            .status = SESSION_CONTACT_RESOLVE_AMBIGUOUS,
            .match_count = matches,
            .exact = exact,
        };
    }
    return finish_unique(contacts, matched_index, matched_field, exact);
}

session_contact_resolution_t session_contact_resolve(
    const session_device_contact_t *contacts,
    size_t count,
    const char *target)
{
    string_slice_t query = trim_target(target);
    if (contacts == NULL || count == 0U || query.length == 0U) {
        return (session_contact_resolution_t) {
            .status = query.length == 0U ? SESSION_CONTACT_RESOLVE_INVALID
                                        : SESSION_CONTACT_RESOLVE_NOT_FOUND,
        };
    }

    size_t id_index = SIZE_MAX;
    size_t id_matches = 0U;
    for (size_t index = 0U; index < count; ++index) {
        if (equals_slice(contacts[index].device_id, query)) {
            id_index = index;
            id_matches++;
        }
    }
    if (id_matches > 1U) {
        return (session_contact_resolution_t) {
            .status = SESSION_CONTACT_RESOLVE_AMBIGUOUS,
            .field = SESSION_CONTACT_MATCH_DEVICE_ID,
            .match_count = id_matches,
            .exact = true,
        };
    }
    if (id_matches == 1U) {
        return finish_unique(contacts,
                             id_index,
                             SESSION_CONTACT_MATCH_DEVICE_ID,
                             true);
    }

    session_contact_resolution_t exact =
        resolve_labels(contacts, count, query, true);
    if (exact.status != SESSION_CONTACT_RESOLVE_NOT_FOUND) {
        return exact;
    }
    return resolve_labels(contacts, count, query, false);
}
