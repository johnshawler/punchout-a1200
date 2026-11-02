sleep 30
xdotool keydown 5; sleep 1.0; xdotool keyup 5
sleep 4
xdotool keydown Control_R; sleep 0.5; xdotool keyup Control_R
sleep 20
for i in 1 2 3 4 5 6; do xdotool keydown Left; sleep 2; xdotool keyup Left; xdotool keydown Right; sleep 2; xdotool keyup Right; done
full p1
for i in 1 2 3 4 5 6; do xdotool keydown Right; sleep 2; xdotool keyup Right; xdotool keydown Control_R; sleep 0.3; xdotool keyup Control_R; xdotool keydown Left; sleep 2; xdotool keyup Left; done
full p2
