#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/cred.h> 
#include <linux/semaphore.h>
#include <linux/uidgid.h>

struct msg {
	unsigned char *message;
	unsigned long length;
	struct list_head msgList;
};

struct mbox{
	unsigned long id;
	int enable_crypt; 
	unsigned long numMsgs;
	struct list_head list;
	struct list_head msgs;  
	//semaphore to protect the list of message which should be changed at the same time as another
	struct semaphore semMsg;
};

//initialization for list containing mailboxes
LIST_HEAD(mailList);
static int numBox = 0;

//semaphore used for locks
//CRITICAL SECTION: List with mailboxes and list of messages for each mailbox
static struct semaphore sem;

// struct used to check for root permissions
static kuid_t rootUser;

/* Make checks for invalid parameters
Creates mailbox objects and links them together in a list
Locked with semaphore so no concurrent access of the mailbox list
*/
SYSCALL_DEFINE2(create_mbox_421,unsigned long, id, int, enable_crypt) {
	struct mbox *pos, *box;
	sema_init(&sem,1);
	rootUser.val = 0;
	//Checks for root, if the ID already exists	
	if (!uid_eq(get_current_cred()->uid, rootUser)) {
		printk("Not Root. Permission denied.");	
		return -EPERM;
	}		
	list_for_each_entry(pos, &mailList,list) {
		printk("id: %lu", pos->id);
		if (pos->id == id) {
			printk("ID already exists");
			return -EEXIST;
		}
	}
	//Makes the mailbox and adds it to the linked list
	box = kmalloc(sizeof(struct mbox), GFP_USER);
	if(!box) {
		printk("Not enough space");
		return -ENOMEM;	
	}			
	box->id = id;
	box->enable_crypt = enable_crypt;
	box->numMsgs = 0;
	sema_init(&(box->semMsg),1);
	INIT_LIST_HEAD(&box->msgs);
	INIT_LIST_HEAD(&box->list);
	//locking
	down_interruptible(&sem);
	list_add(&(box->list), &mailList);
	up(&sem);	

	numBox++;
	return 0;
}
/*
Makes check for bad parameters and any error checking
Removes mailboxes and frees the memory
Locked so no concurrent changed of the mailbox list
*/
SYSCALL_DEFINE1(remove_mbox_421,unsigned long, id) {
	struct mbox *temp;
        struct list_head *pos, *n;
	rootUser.val = 0;
	//check for root, and if the list is empty
	if (!uid_eq(get_current_cred()->uid, rootUser)) {
		printk("Not root. Permission denied.");
		return -EPERM;
	}
	if (numBox == 0) {
		printk("Empty list");
		return -1;
	}	
	//looks for the box(error is doesn't exist) and deletes and frees 
	list_for_each_safe(pos,n, &mailList ) {
		temp = list_entry(pos, struct mbox, list);	
		if (temp->id == id) {
			if (temp->numMsgs > 0) {
				printk("Mailbox not empty.");
				return -ENOTEMPTY;
			}
			//lock			
			down_interruptible(&sem);
			list_del(pos);
			kfree(temp);
			up(&sem);
	
			numBox--;			
			return 0;
		}
	}
	printk("ID doesn't exist");
	return -ENOENT;  
}

//returns number of mailboxes in the list
SYSCALL_DEFINE0(count_mbox_421) { 
	return numBox;
}

/*
Error checks for bad behavior
Copies kernel memory to user memory as an array
Not locked because 
*/
SYSCALL_DEFINE2(list_mbox_421,unsigned long __user *, mbxes, long, k) {
	struct mbox *pos;	
	long total;
	unsigned long byte;
	unsigned long array[k];
	int i;
	total = 0;

	//check for negative length, no mailbox or mbxes is null
	if (k < 0) {
                printk("Bad length.");
                return -1;
        }
	if(numBox == 0) {
                printk("No mailboxes.");
                return -1;
        }
	if(mbxes == NULL) {
		printk("Null pointer");
		return -EFAULT;
	}	
	//send the IDs to an array and coppies them to the user space.
	if(access_ok(VERIFY_WRITE, mbxes, k*(sizeof(unsigned long)))) {	
		list_for_each_entry(pos, &mailList,list) {
			//if the number of mailboxes is greater than K       	        	
			if (total < k) {
				array[total] = pos->id;
				total++;
                	}
			else {
				byte = copy_to_user(mbxes,array,total*sizeof(unsigned long));			
				return (total - byte);
			}
		}
	}
	else {
		printk("Mailbox pointer invalid.");
		return -EFAULT;
        }
	//if the number of mailboxes is less than K
	//This for loop is to the pad the end of the array if the user asks for n boxes but there iso only < n
	for(i = total; i < k; i++) {
		array[i] = 0;
	}
	byte = copy_to_user(mbxes,array,total*sizeof(unsigned long));
	return (total - byte);	
}

SYSCALL_DEFINE4(send_msg_421,unsigned long, id, unsigned char __user *, msg, long, n, unsigned long, key) {
	struct mbox *pos;
	struct msg *mesg;
	unsigned long bytes;
	unsigned char padMsg[n + (n % 4)];
	unsigned char *arr;
	int a;
	//check for negative length, no mailbox or mbxes is null
	if( msg == NULL) {
		return -EFAULT;
	}
	if(numBox == 0) {
                printk("No mailboxes.");
                return -1;
        }
	if( n < 0) {
		printk("Negative length.");
		return -1; 
       	} 
	//looks for the box, if encrypt is on then it encrypts the message
        if(access_ok(VERIFY_READ, msg, n*sizeof(unsigned char))) {		
		list_for_each_entry(pos, &mailList,list) {
               		if (pos->id == id) {
				if(pos->enable_crypt != 0) {
					for(a = 0; a < (n + (n % 4)); a++) {
						if (a < n) {
							padMsg[a] = msg[a];
						}
						else {
							padMsg[a] = '0';
						}
					}
					for(a = 0; a < (n+(n%4)); a++) {
						padMsg[a] = padMsg[a] ^ key;
					}
					for(a = 0; a < n; a++) {
						msg[a] = padMsg[a];
					}
						
				}
				//copies the message to a new dynamically allocated array in kernel space. And connects the message.
				mesg = kmalloc(sizeof(struct msg), GFP_USER);
				arr = kmalloc(sizeof(unsigned char) * n, GFP_USER);			
				mesg->message = arr;				
				bytes = copy_from_user(mesg->message, msg, n*sizeof(unsigned char)); 
				printk("Bytes note written = %lu \n", bytes);
				if(bytes != 0) {
					printk("Message could not be copied from user");				
					return -EFAULT;
				}
				mesg->length = n - bytes;
				INIT_LIST_HEAD(&mesg->msgList);
				down_interruptible(&pos->semMsg);
				list_add_tail(&(mesg->msgList),&pos->msgs);
				up(&pos->semMsg);
				pos->numMsgs++;
				printk("numMsgs: %lu\n", pos->numMsgs);		
		
				return (n - bytes);
			}
		}
        }
	else {
        	printk("Message pointer is invalid");
                return -EFAULT;
        }

        printk("ID doesn't exist");
        return -ENOENT;

}

SYSCALL_DEFINE4(recv_msg_421,unsigned long, id, unsigned char __user *, msg, long, n, unsigned long, key) {
	struct mbox *pos;
	struct msg *temp;
	struct list_head *msgPos, *posTemp;
	unsigned long byte;
	int a;
	//check for negative length, no mailbox or msg is null
        if( msg == NULL) {
		return -EFAULT;
	}
	if( n < 0) {
                printk("Negative length.");
                return -1;
        }
	if(numBox == 0) {
		printk("No mailboxes.");
		return -1;
	}	
	//checks for the box, and decrypts if the mailbox enables encryption
	if(access_ok(VERIFY_WRITE, msg, n*sizeof(unsigned char))) {
        	list_for_each_entry(pos, &mailList,list) {
               		if (pos->id == id) {
				if(pos->numMsgs == 0) {
					printk("There are no messages to recieve.");
					return -1;
				}
				
				//copies the message from keneral space to user space and frees data
				list_for_each_safe(msgPos,posTemp,&pos->msgs) {
					temp = list_entry(msgPos, struct msg, msgList);	
					if(n < temp->length) {
						printk("Given space is not enough for msg");
						return -1;
					}
					if(pos->enable_crypt != 0) {
						unsigned long pad = temp->length + (temp->length % 4);
						unsigned char padMsg[pad];
						for(a = 0; a < pad; a++) {
							if (a < n) {
								padMsg[a] = temp->message[a];
							}
							else {
								padMsg[a] = '0';
							}
						}
						for(a = 0; a < pad; a++) {
							padMsg[a] = padMsg[a] ^ key;
						}
						for(a = 0; a < temp->length; a++) {
							temp->message[a] = padMsg[a];
						}
					}	
				
					byte = copy_to_user(msg, temp->message, temp->length*sizeof(unsigned char));
					down_interruptible(&pos->semMsg);
					list_del(msgPos);
					kfree(temp->message);					
					kfree(temp);
					up(&pos->semMsg);					
					pos->numMsgs--;	
					return(n - byte);	
						
				}
               		}
			else {
				printk("Message pointer is invalid");
				return -EFAULT;
			}
		}
        }
	printk("ID doesn't exist");
        return ENOENT;
}

SYSCALL_DEFINE4(peek_msg_421,unsigned long, id, unsigned char __user *, msg, long, n, unsigned long, key) {
	struct mbox *pos;
	struct msg *mesg;
	unsigned long byte;
	int a;
	//check for negative length, no mailbox or msg is null
	if( msg == NULL) {
		return -EFAULT;
	}
	if(numBox == 0) {
                printk("No mailboxes.");
                return -1;
        }

	if( n < 0) {
                printk("Negative length.");
                return -1;
        }
	//checks for the box, and decrypts if the mailbox enables encryption
	if(access_ok(VERIFY_WRITE, msg, n*sizeof(unsigned char))) {
        	list_for_each_entry(pos, &mailList,list) {
                	if (pos->id == id) {
				if(pos->numMsgs == 0) {
                       	        	printk("There are no messages to recieve.");
                                	return -1;
                        	}
				mesg = list_first_entry(&pos->msgs, struct msg, msgList);
				if(n < mesg->length) {
					printk("Given space is not enough for msg");
					return -1;
				}
				if(pos->enable_crypt != 0) {
					unsigned long pad = mesg->length + (mesg->length % 4);
					unsigned char padMsg[pad];
					for(a = 0; a < pad; a++) {
						if (a < n) {
							padMsg[a] = mesg->message[a];
						}
						else {
							padMsg[a] = '0';
						}
					}
					for(a = 0; a < pad; a++) {
						padMsg[a] = padMsg[a] ^ key;
					}
					for(a = 0; a < mesg->length; a++) {
						mesg->message[a] = padMsg[a];
					}
				}
				//copies the message from keneral space to user space and frees data
				byte = copy_to_user(msg, mesg->message, (mesg->length)*sizeof(unsigned char));
				if(byte != 0) {
					printk("Something went wrong when copying.");
					return -1;
				}
				return (n - byte);
			}
			else {
				printk("Message pointer is invalid");
				return -EFAULT;
			}
                }
        }
        printk("ID doesn't exist");
        return -ENOENT;

}

SYSCALL_DEFINE1(count_msg_421,unsigned long, id) {
	struct mbox *pos;
	unsigned long count;
	if(numBox == 0) {
                printk("No mailboxes.");
                return -1;
        }
	list_for_each_entry(pos, &mailList,list) {
                if (pos->id == id) {
			count = pos->numMsgs; 
			return count; 
                }
        }
	printk("ID doesn't exist");
	return -ENOENT;
}

SYSCALL_DEFINE1(len_msg_421,unsigned long, id) {
	struct mbox *pos;
	struct msg *first;
	unsigned long length;
	//checks if mailbox list is empty
        if(numBox == 0) {
                printk("No mailboxes.");
                return -1;
        }
	//looks for the box and returns the length of the first message
	list_for_each_entry(pos, &mailList,list) {
                if (pos->id == id) {
			if(pos->numMsgs == 0) {
                                printk("There are no messages.");
                                return -1;
                        }
			first = list_first_entry(&pos->msgs, struct msg, msgList);
			length = first->length;
			return length;
		}
        }
        printk("ID doesn't exist");
        return -ENOENT;
}
