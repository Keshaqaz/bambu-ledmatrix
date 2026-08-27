#pragma once

// Public half of the project OTA signing key. The private key is local-only
// under .secrets/ and must never be committed or copied to the device.
static const char OTA_SIGNING_PUBLIC_KEY[] =
  "-----BEGIN PUBLIC KEY-----\n"
  "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEpNInn+9/406a1mM7DjMVeGbRtY+j\n"
  "9pUVD4TQHhd2eCWSH+1t7n6kCROcxLvOOKN9wsVpQWq6HYYdei1oOA1piw==\n"
  "-----END PUBLIC KEY-----\n";
