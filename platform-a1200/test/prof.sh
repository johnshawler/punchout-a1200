# profile run (build with EXTRA="-DPROFILE -DHUD -DPSH=7 -DPLIM=0x44000"): the game must be playing during ticks 2400..3300, the profile appears at tick 3500
sleep 20
xdotool keydown 5; sleep 1.0; xdotool keyup 5
sleep 3
xdotool keydown Control_R; sleep 0.5; xdotool keyup Control_R
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do xdotool keydown Left; sleep 2; xdotool keyup Left; xdotool keydown Right; sleep 2; xdotool keyup Right; done
for n in 1 2 3 4; do sleep 12; full p$n; done
