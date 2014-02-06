#Start example .gdbinit file.
dir $HOME/csc369/a2-starter/src/kern/compile/ASST2
# Above line tells the debugger the path to the location where the code was compiled. Remember to edit the path to match your source location!

#Define a macro to connect to waiting kernel, to save typing on each startup
define db
target remote unix:.sockets/gdb
# Above line connects debugger to the waiting kernel
end


define syscall_time
# Defines a macro for GDB to use called "syscall_time"
break syscall
# Sets a breakpoint on a function called "syscall"
break sys___time
end
# End macro definition


define interrupt_b
break common_exception
break mips_trap
break mips_interrupt
break exception_return
end

# End .gdbinit

