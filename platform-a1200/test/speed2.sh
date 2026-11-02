sleep 30
full a0
xdotool keydown 5; sleep 1.0; xdotool keyup 5
sleep 4
xdotool keydown Control_R; sleep 0.5; xdotool keyup Control_R
for n in 1 2 3 4 5 6; do sleep 10; xdotool keydown Left; sleep 3; xdotool keyup Left; full g$n; done
