# A syntax error ends a script with status 2, after the earlier lines ran.
echo before
if true; then echo inside
