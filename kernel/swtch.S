# Context switch
#
#   void swtch(struct context *old, struct context *new);
# 
# Save current registers in old. Load from new.	
#  将当前cpu上的reg保存在old 上，并将new 加载到cpu的reg上



.globl swtch
swtch:
        # store from old
        sd ra, 0(a0)    # return addr 
        sd sp, 8(a0)    # kernel thread stack pointer
        sd s0, 16(a0)   # 被调用者保存的寄存器
        sd s1, 24(a0)
        sd s2, 32(a0)
        sd s3, 40(a0)
        sd s4, 48(a0)
        sd s5, 56(a0)
        sd s6, 64(a0)
        sd s7, 72(a0)
        sd s8, 80(a0)
        sd s9, 88(a0)
        sd s10, 96(a0)
        sd s11, 104(a0)

        # load from new
        ld ra, 0(a1)    # return addr (scheduler 的swtch的下一行)
        ld sp, 8(a1)    # scheduler thread stack pointer
        ld s0, 16(a1)   # 被调用者保存的寄存器
        ld s1, 24(a1)
        ld s2, 32(a1)
        ld s3, 40(a1)
        ld s4, 48(a1)
        ld s5, 56(a1)
        ld s6, 64(a1)
        ld s7, 72(a1)
        ld s8, 80(a1)
        ld s9, 88(a1)
        ld s10, 96(a1)
        ld s11, 104(a1)
        # 这里的ret什么时候执行？当再次切换回来的时候执行？ret到哪里？
        # 上面那行注释理解的不对。ret就是和整个swtch.S顺序执行的。
        # 只是此时ret到的地址 不再是刚才进入swtch.S时的ra
        # ra已经被改变，改成了scheduler地址 
        ret

	
