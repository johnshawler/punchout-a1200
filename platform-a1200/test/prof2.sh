sleep 25
xdotool keydown 5; sleep 1.0; xdotool keyup 5
sleep 3
xdotool keydown Control_R; sleep 0.5; xdotool keyup Control_R
keys=(Left Right Up Down Left Right)
for i in $(seq 1 40); do
  k=${keys[$((RANDOM % 6))]}
  xdotool keydown $k; sleep 0.$((RANDOM % 9 + 1)); xdotool keyup $k
  if [ $((RANDOM % 3)) -eq 0 ]; then xdotool keydown Control_R; sleep 0.2; xdotool keyup Control_R; fi
done
sleep 50
full p1
sleep 10
full p2
