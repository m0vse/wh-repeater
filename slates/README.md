# Editable Slate Templates

These files are the first step toward HTML-rendered repeater slates.

The current media output still uses the built-in C++/FFmpeg drawtext renderer.
The selected first HTML renderer is packaged headless Chromium, wrapped by
`tools/render-slate-html`. The daemon should render templates to cached PNGs
only when slate content changes, then feed those bitmaps through the existing
generated video path.

Template set:

- `sleep.html`
- `signal-received.html`
- `post-access.html`
- `diagnostic.html`
- `testcard.html`
- `style.css`

Initial variables:

- `{{callsign}}`
- `{{receiver_suffix}}`
- `{{input_label}}`
- `{{frequency_khz}}`
- `{{symbol_rate_ks}}`
- `{{service_line}}`
- `{{video_format}}`
- `{{source_line}}`
- `{{rf_line}}`
- `{{modulation}}`
- `{{sleep_start}}`
- `{{sleep_end}}`

Rules for the renderer:

- Render at the configured output size.
- Render only when template data changes.
- Never run HTML layout in the per-frame output loop.
- Avoid JavaScript and remote assets.
- On render failure, continue output using the last good bitmap or built-in
  fallback slate.

Manual render test:

```sh
render-slate-html /etc/wh-pc-gateway/slates/testcard.html /tmp/testcard.png 1280 720
```
