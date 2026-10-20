# Android ROM file access

Use **Browse** and select your own `daytona93.zip` or `.7z`. Android grants the
app read access to that document. A `content://...` value is a document URI,
not a filename: it must not be decoded into a `/sdcard/...` path or opened
with `std::ifstream`.

The launcher now opens the selected URI read-only with `SDL_IOFromFile`.
It streams the bytes into an app-private temporary file, runs the existing
`check_rom_set` validation, and only then replaces `imported-daytona93.rom`
in the SDL preferences directory. The runtime detects ZIP/7z by signature,
so the internal `.rom` suffix does not change the archive format. The saved
configuration points to this ordinary local file; future launches and resets
do not depend on the document provider's temporary URI grant.

Failed reads and failed verification discard the temporary file. The existing
import is not replaced until validation and copying succeed. Copying uses a
64 KiB buffer and rejects archives larger than 512 MiB. Only the selected
archive is imported; its original is neither changed nor deleted. Replacing
that original later does not update the app's copy: select it again.

No `MANAGE_EXTERNAL_STORAGE`, `READ_MEDIA_*` or blanket storage grant is
required for this picker-based flow. `READ_EXTERNAL_STORAGE` alone would not
fix a URI being passed to the native filesystem reader, and has no effect on
Android 13/API 33 and higher. Do not add broad permissions or strip the URI.

## Update an existing installation

From the repository root, on branch `mobile`:

```sh
git pull --ff-only origin mobile
cd platform/mobile/android
gradle :app:assembleDebug
adb -s DEVICE_SERIAL install -r app/build/outputs/apk/debug/app-debug.apk
```

Open the app, tap **Browse** and select the ROM archive again. This refreshes
read access if a previously saved URI grant has expired. There is no need to
uninstall the app or clear its data. A successfully checked import changes the
ROM field from the `content://...` URI to the app-private `.rom` file.

`adb push` can still be used to put the user's archive in Downloads first:

```sh
adb -s DEVICE_SERIAL push /path/to/daytona93.zip /sdcard/Download/
```

Replace `DEVICE_SERIAL` with the target serial from `adb devices`.
The app gets access by selecting that file through Browse, not because ADB
copied it there.

## ROM-free tests

```sh
bash tests/test_mobile_rom_file.sh
CXX=clang++ bash tests/test_mobile_rom_file.sh
```

These test short/partial reads, byte preservation, keeping the old import
until commit, failed opens, empty streams, read/close errors, output/rename
failures, cleanup, opaque URI IDs and the ordinary desktop path. They use a
small SDL stream shim on the host. They are not an Android APK build or a test
of a real device's permission grant or document provider.

References:
- https://developer.android.com/training/data-storage/shared/documents-files
- https://developer.android.com/reference/android/Manifest.permission#READ_EXTERNAL_STORAGE
- https://wiki.libsdl.org/SDL3/SDL_IOFromFile
