/**
 * @file AgbGuestRegistry.cpp
 * @brief Games built as wasm2c guests register here at startup (AgbGuestApi.h).
 */
#include "AgbGuestApi.h"

#include <string.h>

static const AgbGuestDesc* sGuests[16];
static int sGuestCount;

extern "C" void AgbRegisterGuest(const AgbGuestDesc* desc)
{
    if (desc == nullptr || desc->api_version != AGB_GUEST_API_VERSION || sGuestCount >= 16)
    {
        return;
    }
    sGuests[sGuestCount++] = desc;
}

extern "C" const AgbGuestDesc* AgbFindGuest(const char* package)
{
    for (int i = 0; i < sGuestCount; ++i)
    {
        if (strcmp(sGuests[i]->package, package) == 0)
        {
            return sGuests[i];
        }
    }
    return nullptr;
}
