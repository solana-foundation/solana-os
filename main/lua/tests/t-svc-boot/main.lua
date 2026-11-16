-- Regression test (not shipped): svc-run.ts writes services.json as a badge left it
-- after restarting in the middle of this app's service start ("starting": true), and
-- checks the next boot skips it, and turns starting at boot off after three.
ui.label(8, 8, "t-svc-boot", {size = 12})
