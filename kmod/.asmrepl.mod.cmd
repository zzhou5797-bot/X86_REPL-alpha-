savedcmd_asmrepl.mod := printf '%s\n'   asmrepl.o | awk '!x[$$0]++ { print("./"$$0) }' > asmrepl.mod
