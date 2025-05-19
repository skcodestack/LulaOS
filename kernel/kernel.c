#include <tty/tty.h>
void _kernel_init(){

}

void _kernel_main(void* info_table){
    tty_put_string("Hello World!/n");
}