# Compound storage and MSI fixtures

`make_fixture.py` deterministically creates CFB v3/v4 files with FAT, MiniFAT, nested storage and independently specified payload bytes.

`make_msi.py` authors `microsoft.msi` using the Windows Installer database APIs through ctypes. It creates Property, Numbers, CustomAction and Media tables, and embeds the owned CAB fixture. It never installs/registers a product or executes an action. The CustomAction row is deliberately named NeverRun and has no sequence invoking it. The reader itself does not link MSI/COM.

The package is a database fixture, not a valid deployable application. Preserve the generated MSI for non-Windows CI.
