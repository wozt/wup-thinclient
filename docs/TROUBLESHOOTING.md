# Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Menu stuck on "waiting" | wrong IP / server down | Y → check IP, `docker compose ps` server side |
| `*.*.*.*` connects to wrong machine | "any server" mode | Y → set explicit IP |
| Black screen after connect | keyframe wait / NACK storm | wait 2 s; check server logs |
| App lockup | decoder desync | HOME+B quit, relaunch |
| No audio | server PulseAudio null-sink misconfigured | see ThinServer docs/TROUBLESHOOTING |
| Input frozen | server released inputs on disconnect | reconnect |

Diagnostics: the app keeps the upstream flight-recorder diag thread; crash
dumps go to the Homebrew Menu crash handler.
