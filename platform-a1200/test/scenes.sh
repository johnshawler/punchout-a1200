# shots every 4 s over attract (45 s), then play (about 100 s): HUD speed per scene
for i in $(seq 1 11); do sleep 4; full a$i; done
xdotool keydown 5; sleep 1.0; xdotool keyup 5
sleep 3
xdotool keydown Control_R; sleep 0.5; xdotool keyup Control_R
keys=(Left Right Up Down Left Right)
for n in $(seq 1 30); do
  for i in 1 2 3 4; do k=${keys[$((RANDOM % 6))]}; xdotool keydown $k; sleep 0.$((RANDOM % 9 + 1)); xdotool keyup $k; if [ $((RANDOM % 3)) -eq 0 ]; then xdotool keydown Control_R; sleep 0.2; xdotool keyup Control_R; fi; done
  full b$n
done
