# Templates

This folder holds checked-in blank versions of files that are otherwise
gitignored because they contain machine/environment-specific values (WiFi
credentials, and anything similar added later — per-satellite secrets, etc.).

**Convention:** each file's path under `templates/` mirrors its real
destination path in the repo. To (re)create a real config file — first
setup on a new machine, or an existing value changing (e.g. moving house,
new WiFi) — copy it from its template path to the matching real path, then
edit the real file with actual values:

```bash
# example: reset the hub's WiFi credentials after moving/changing routers
cp templates/firmware/hub/include/secrets.h firmware/hub/include/secrets.h
# then edit firmware/hub/include/secrets.h with the new SSID/password
```

Real files at those destination paths are gitignored (see `.gitignore`);
only the templates here are committed.

## Current templates

| Template | Real location | Contains |
|---|---|---|
| `firmware/hub/include/secrets.h` | `firmware/hub/include/secrets.h` | Hub WiFi SSID/password |
