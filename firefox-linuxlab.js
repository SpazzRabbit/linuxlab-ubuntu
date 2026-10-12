// Linux Lab: defaults for Firefox, see README.md of /opt/linuxlab-gnome ("Video in Firefox").
// Pages are asked to go easy on animation. Firefox draws with the CPU here, and an animation that
// keeps a large part of a page changing takes every frame of a video on that page with it.
pref("ui.prefersReducedMotion", 1);
// Firefox on Wayland takes the frame callbacks of the compositor for the vertical blank and draws
// a video frame 5 to 10 ms after each; here that time is what decides which refresh of the screen
// a frame lands on, and the video judders. With its own 60 Hz timer the frames come evenly, and the
// bridge shows them on the beat.
pref("widget.wayland.vsync.enabled", false);
