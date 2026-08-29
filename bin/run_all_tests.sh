for f in $(ls Run_*Tester* | sort); do [ -x "$f" ] && echo "=== Running $f ===" && ./"$f" || exit 1; done
