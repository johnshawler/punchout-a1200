sleep 30
xdotool keydown 5; sleep 1.0; xdotool keyup 5
sleep 3
xdotool keydown Control_R; sleep 0.5; xdotool keyup Control_R
keys=(Left Right Up Down Left Right)
for round in 1 2 3 4 5 6 7 8; do
  for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
    k=${keys[$((RANDOM % 6))]}
    xdotool keydown $k; sleep 0.$((RANDOM % 9 + 1)); xdotool keyup $k
    if [ $((RANDOM % 3)) -eq 0 ]; then xdotool keydown Control_R; sleep 0.2; xdotool keyup Control_R; fi
  done
  full r$round
  if [ $round -eq 4 ]; then xdotool keydown 5; sleep 1; xdotool keyup 5; fi
done
