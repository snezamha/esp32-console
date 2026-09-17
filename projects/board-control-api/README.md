# Board control API

Install **Board control API v1.0.5** from Projects on an ESP32-S3 LCD 0.85 running base firmware 1.1.27 or newer. The project does not draw text on the board display.

Open the **Board API** tab next to **Project Builder** for the endpoint, token management, JSON request workbench, LED simulator, examples, and the full API guide. The token is sent in the `Authorization: Bearer <token>` header. It is separate from the board pairing token.

The API controls display, power, sound, Bluetooth, clock, and LED settings, including per-LED RGB values. Speaker commands support beep, MP3 URL or uploaded MP3, pause, play, and stop. Play uses `https://navairan.com/;stream.nsv` when no previous music source is saved. Because music is streamed, play after pause starts from the beginning of the track or stream. API GET reads the console database and does not contact the board.

POST requests are serialized per board. A completed action returns HTTP 200 with `success: true`, `status: "done"`, and a message from the board. For music this means playback reached Playing; for a beep it means its samples finished. When the board has not confirmed within the HTTP wait, POST returns HTTP 202 with `requestId`. Poll `GET <endpoint>?requestId=<requestId>` with the same Bearer token until it returns 200 or 409. Requests that cannot connect or receive an acknowledgment within 90 seconds fail, allowing the next queued request to run. The Board API workbench polls automatically and keeps Send request disabled until the current action completes.
