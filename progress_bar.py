"""
General form: \033[<attrs>;<fg>;<bg>mYour text\033[0m
    \033 is ESC
    <attrs> = style (optional)
    <fg> = foreground (text) color
    <bg> = background color
    \033[0m resets everything.

  attributes
  1m = Bold , 2m = Dim ,3m = Italic (if supported), 4m = Underline, 7m = Inverse, 9m = Strikethrough

  fg:
    30m = Black , 31m = Red, 32m = Green , 33m = Yellow , 34m = Blue , 35m = Magenta, 36m = Cyan, 37m = White, 
    90m  =  Bright Black (gray), 91m = Bright Red ,92m = Bright Green, 93m = Bright Yellow, 94m = Bright Blue, 95m = Bright Magenta, 96m = Bright Cyan, 97m  Bright White
  bg 
    40m = Black , 41m = Red, 42m = Green , 43m = Yellow , 44m = Blue , 45m = Magenta, 46m = Cyan, 47m = White, 
    100m  = gray , 101m = Bright Red ,102m = Bright Green, 103m = Bright Yellow, 104m = Bright Blue, 105m = Bright Magenta, 106m = Bright Cyan, 107m  Bright White

"""

import time
import sys

def progress_bar(iteration, total, length=40):
    percent = ("{0:.1f}").format(100 * (iteration / float(total)))
    filled_length = int(length * iteration // total)
    bar = '█' * filled_length + '-' * (length - filled_length)
    sys.stdout.write(f'\rProcessing: [{bar}] {percent}% Complete')
    sys.stdout.flush() # Force the output to appear immediately

# Example usage:
total_steps = 50
for i in range(total_steps + 1):
    progress_bar(i, total_steps)
    time.sleep(0.1) # Simulate a task
print("\nDone!") # Print a newline character when finished
