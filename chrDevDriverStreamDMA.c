#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include "pcieDriver.h"
#include "chrDevDriver.h"
#include "Regs.h"

/* 调试信息输出控制. */
#define DEBUG
#ifdef DEBUG
#define PCIE_DEBUG(fmt, args...) printk("PCIe: " fmt, ##args)
#else 
#define PCIE_DEBUG(fmt, args...)
#endif

/*********************************************************************/
union writeBytes {
	unsigned short shortInfo;
	char bytesInfo[2];
};

union readBytes {
	unsigned short shortInfo;
	char bytesInfo[2];
};

struct ioctlUsrInfo {
	unsigned int regVal;
	int regSerial;
};
/********************************************************************/

/*
	函数名：pcieCardOpen
	参　数：struct inode*, strcut file*;
	返回值：成功打开字符设备 返回 0；
	函数功能：打开用于操作PCIe设备而注册的字符设备,并将"PCIe设备私有数据结构"传递给字符设备文件描述中的flip->private_data字段。
 * */
int pcieCardOpen( struct inode * inode, struct file *flip)
{
	struct pcieCardPrivate * card = ( struct pcieCardPrivate *) container_of(
											inode->i_cdev, struct pcieCardPrivate,
											cdev);
	flip->private_data = card;
	return 0;
}

/*
	函数名：pcieDeviceInit
	参　数：struct pcieCardPrivate *( SPTR)
	返回值：完成对PCIe设备的初始化 返回0
	函数功能：初始化接入到操作系统中的PCIe设备, 复位设备，设置总线宽度，配置工作模式选择，开启模式对应的中断使能。
*/
static int pcieDeviceInit ( struct pcieCardPrivate * cardPrivateData)
{
	unsigned int setValue,chkValue;
#ifdef TEST
	cardPrivateData->writeCountor = cardPrivateData->intrReadCountor = 0;
#endif
	/* 设备复位 与总线宽度、设备工作模式选择 */
	setValue = chkValue = 0;
	setValue |= ( DEVICE_RESET_CTRL|DEVICE_WIDTH_SELECT|DEVICE_MODE_SELECT);
	iowrite32( setValue, cardPrivateData->bar0MapedAddr + DCSR0);
	chkValue = ioread32( cardPrivateData->bar0MapedAddr + DCSR0);
	PCIE_DEBUG( "DCSR0: setValue:%x,chkValue:%x\n",setValue,chkValue);

	/* 延时一段时间，再次写入该值 */
	setValue = chkValue = 0;
	setValue |= ( DEVICE_WIDTH_SELECT | DEVICE_MODE_SELECT);
	iowrite32( setValue, cardPrivateData->bar0MapedAddr + DCSR0);
	chkValue = ioread32( cardPrivateData->bar0MapedAddr + DCSR0);
	PCIE_DEBUG( "DCSR0: setValue:%x,chkValue:%x\n",setValue,chkValue);
	/* 中断管理 
		使能DMA传输完成中断、使能DMA写请求中断	
	*/
	setValue = chkValue = 0;
	setValue |= ( ENINTR_READ_COMPLETE | ENINTR_WRITE_COMPLETE);
	iowrite32( setValue, cardPrivateData->bar0MapedAddr + DCSR1);
	chkValue = ioread32( cardPrivateData->bar0MapedAddr + DCSR1);	
	PCIE_DEBUG( "DCSR1: setValue:%x,chkValue:%x\n",setValue,chkValue);
	
	setValue = chkValue = 0;
	setValue |= ENINTR_WRITE_REQ;
	iowrite32( setValue, cardPrivateData->bar0MapedAddr + DCSR2);
	chkValue = ioread32( cardPrivateData->bar0MapedAddr + DCSR2);
	PCIE_DEBUG( "DCSR2: setValue:%x,chkValue:%x\n",setValue, chkValue);
	
	iowrite32( SET_TRANS_MAX_SIZE, cardPrivateData->bar0MapedAddr + DMA_TRANS_MAX_SIZE);	
	chkValue = ioread32( cardPrivateData->bar0MapedAddr + DMA_TRANS_MAX_SIZE);
	PCIE_DEBUG( " SET_TRANS_MAX_SIZE-->%x\n", chkValue);
	return 0;
}

/* 
	函数名：createReadResource
	参　数：struct pcieCardPrivate *
	返回值：　-1 PCIe设备驱动程序开辟readResource失败
		　正整数　表示成功分配的ReadResource数目
	函数功能：开辟PCIe设备驱动程序用于以DMA方式接受设备数据的内存空间
*/
static int createReadResource ( struct pcieCardPrivate * cardPrivateData) 
{
	/* 循环变量，循环申请用于DMA方式进行数据传输的内存资源. */
	int cr1 = 0;
	struct pktRead * tempPktPointer, * curPktPointer = NULL;
	for ( cr1 = 0; cr1 < PKT_READ_RESOURCE_NUM; cr1++) {
#if DMA_BUFFER_SIZE_128K
		PCIE_DEBUG("kmalloc -- \n");
		tempPktPointer = ( struct pktRead *) kmalloc( sizeof( struct pktRead), /*GFP_DMA32*/GFP_KERNEL);
		tempPktPointer->data = kmalloc( MWR_DMA_PKT_SIZE, GFP_DMA32);	
#else
		PCIE_DEBUG( "__get_free_pages--%d\n", MWR_DMA_PKT_ORDER);
		// tempPktPointer = ( struct pktRead *) __get_free_pages( GFP_KERNEL, MWR_DMA_PKT_ORDER);
		tempPktPointer = ( struct pktRead *)kmalloc( sizeof( struct pktRead), GFP_KERNEL);
		tempPktPointer->data = ( void *) __get_free_pages( GFP_DMA32, MWR_DMA_PKT_ORDER);
#endif
		PCIE_DEBUG("Mem pages addr:%p\n,%p\n", (void *) tempPktPointer,tempPktPointer->data);
		if ( (tempPktPointer == NULL) || ( tempPktPointer->data == NULL)) {
			printk( "Can not alloc mem for pktRead.\n");
			return -1;
		}
		if ( cr1 == 0)
			cardPrivateData->readResource = tempPktPointer;
		else 
			curPktPointer->next = tempPktPointer;
		curPktPointer = tempPktPointer;
		tempPktPointer->next = cardPrivateData->readResource;
	}
		cardPrivateData->RD_readResource = cardPrivateData->WR_readResource = cardPrivateData->readResource;
		PCIE_DEBUG( "Create Read Resource succeded.\n");
	return PKT_READ_RESOURCE_NUM;
}

/*
	函数名：createWriteResource
	参　数：struct pcieCardPrivate * (SPTR)
	返回值：-1 申请用于DMA数据传输的内存空间失败
		正正数： 申请的用于DMA数据传输的内存空间资源的数目
	函数功能：向操作系统申请用于PCIe设备DMA写的内存空间。
*/
static int createWriteResource ( struct pcieCardPrivate * cardPrivateData)
{

	int cr1 = 0;
	struct pktWrite * tempPktPointer, * curPktPointer = NULL;
	for ( cr1 = 0; cr1 < PKT_WRITE_RESOURCE_NUM; cr1++) {
#if DMA_BUFFER_SIZE_128K
		PCIE_DEBUG("kmalloc -- \n");
		tempPktPointer = ( struct pktWrite *) kmalloc( sizeof( struct pktWrite), GFP_KERNEL);
		tempPktPointer->data = kmalloc( MRD_DMA_PKT_SIZE, GFP_DMA32);
#else
		PCIE_DEBUG( "__get_free_pages--%d\n", MWR_DMA_PKT_ORDER);
		tempPktPointer = ( struct pktWrite *) kmalloc( sizeof( struct pktWrite), GFP_KERNEL);
		tempPktPointer->data = ( void *)__get_free_pages( GFP_DMA32, MRD_DMA_PKT_ORDER);
#endif
		PCIE_DEBUG("Mem pages addr:%p, --%p\n", (void *) tempPktPointer, tempPktPointer->data);
		if ( (tempPktPointer == NULL) || (tempPktPointer->data == NULL)) {
			printk( "Can not alloc mem for pktRead.\n");
			return -1;
		}
		if ( cr1 == 0)
			cardPrivateData->writeResource = tempPktPointer;
		else 
			curPktPointer->next = tempPktPointer;
		curPktPointer = tempPktPointer;
		tempPktPointer->next = cardPrivateData->writeResource;
	}
		cardPrivateData->RD_writeResource = cardPrivateData->WR_writeResource = cardPrivateData->writeResource;
		PCIE_DEBUG( "Create Write Resource succeded.\n");
	return PKT_WRITE_RESOURCE_NUM;
}


/* 
	函数名：pcieCardRelease
	参  数：struct inode *, struct file *;
	返回值：0 成功
	函数功能：适当时候将设设备注销掉
 * */
int pcieCardRelease ( struct inode * inode, struct file *flip)
{
	flip->private_data = NULL;
	return 0;
}	

/*
 *	函数名：pcieCardRead
	参　数：
	返回值：　读取成功，返回值为0 *	
		  读取数据失败，返回值为一个负数　
			-1 读取pcie设备是否可读信息失败　
			-2 获取本次所读取数据的长度信息错误 		
	函数功能： 读取pcie设备上的数据
 * */
ssize_t pcieCardRead( struct file * file, char __user * buff, size_t count, loff_t *f_ops) 
{

	/* *	通过注册的字符设备，对接入的PCIe设备进行数据的读取* */
	 
	/* 获取板卡的私有数据结构 struct pcieCardPrivate * cardPrivateData */
	struct pcieCardPrivate * cardPrivateData = (struct pcieCardPrivate *) file->private_data;
	//unsigned int deviceCtlRegVal = 0;
	//unsigned int dmaCtlRegVal = 0; 
	unsigned int DCSR2Val;
	//int lenOfDataFromDev;
	/* 6.21-modify--调试打印信息.  */
	if ( down_interruptible( &cardPrivateData->pcieDeviceSem)) {
		printk( " pcieDeviceSem opt error.\n");
		return 0;
	}
	PCIE_DEBUG("pcieCardRead:enter pcieCardRead fuc\n");	
	/* 读取ＤＭＡ传输的数据长度，是否需要字节序转换。 */
	PCIE_DEBUG("pcieCardRead:readEnableFlag:%d.\n",atomic_read(&cardPrivateData->readEnableFlag));
	if (wait_event_interruptible( cardPrivateData->readQueue, ( atomic_read( &cardPrivateData->readEnableFlag) > 0))) {
		printk(" wait_event_interruptible failure.\n");
		return -1;
	}
	
//	PCIE_DEBUG("pcieCardRead:readEnableFlag:%d.\n",atomic_read(&cardPrivateData->readEnableFlag));
/*6.21--modify-dec ReadEnableFalg*/
	atomic_dec( &cardPrivateData->readEnableFlag);
	//PCIE_DEBUG("pcieCardRead:readEnableFlag dec:%d.\n", atomic_read(&cardPrivateData->readEnableFlag));
#if 0
/* 6-17 print recv info */
	for ( crPrint = 1020; crPrint < 1024; crPrint++) {
		PCIE_DEBUG("-->%X\n", cardPrivateData->RD_readResource->data[crPrint]);
	}
#endif
	PCIE_DEBUG("pcieCardRead:After wait_event_inter.\n");
#if 0
	/* 检查从设备读取数据的长度信息是否有错误 */
	if ( (count == 0) || ( count < cardPrivateData->RD_readResource->dataLength)) {
		printk(" PCIE device driver pcieCardRead error( about data length.)\n");
		return -1
	} else
		count = cardPrivateData->RD_readResource->dataLength;
	PCIE_DEBUG("pcieCardRead:count:%d.\n",count);
#endif
#ifdef	DEVICE_SET_MWR_DMA_LENGTH 
	if ( cardPrivateData->RD_readResource->dataLength > count) {
		printf( " PCIe DMA translate Data lenth error.\n");
		up( cardPrivateData->pcieDeviceSem);
		return -1;
	}

	if ( copy_to_user( buff, cardPrivateData->RD_readResource->data, count) ) {
		printk("DMA read failure.\n");
		return -1;
	}
#endif 
	PCIE_DEBUG("pcieCardRead:readResource->data Addr:%p.\n", cardPrivateData->RD_readResource->data);
	if ( copy_to_user( buff, cardPrivateData->RD_readResource->data, cardPrivateData->RD_readResource->dataLength)) {
		printk( " copy_from_user failure.\n");
		return -1;
	}	
	cardPrivateData->RD_readResource = cardPrivateData->RD_readResource->next;
	PCIE_DEBUG("pcieCardRead:pktReadFree:%d.\n",atomic_read(&cardPrivateData->pktReadFree));
	//spin_lock(&cardPrivateData->codeProtect);
/* 6.23-modigy 手动调试*/
	if ( atomic_inc_return( &cardPrivateData->pktReadFree)==1 ) {
#if 0
		/* 测试 */
		do { 
			if (atomic_read( &cardPrivateData->intrMaskConfig) ==1)
				break;
		} while(1);
#endif
		down(&cardPrivateData->intrMaskConfig);
		//atomic_dec( &cardPrivateData->pcieMaskConfig);
	//	PCIE_DEBUG("pcieCardRead:release MASK_WRITE_REQ.\n");
		DCSR2Val = ioread32( cardPrivateData->bar0MapedAddr + DCSR2);
	//	PCIE_DEBUG("pcieCardRead:DCSR2Val:before:%X.\n",DCSR2Val);
		DCSR2Val &= (~MASKINTR_WRITE_REQ);
		PCIE_DEBUG("pcieCardRead:DCSR2Val:dealing:%X.\n",DCSR2Val);
		iowrite32( DCSR2Val, cardPrivateData->bar0MapedAddr + DCSR2);
	//	DCSR2Val = ioread32( cardPrivateData->bar0MapedAddr + DCSR2);
	//	PCIE_DEBUG("pcieCardRead:DCSR2Val:after:%X.\n",DCSR2Val);
		PCIE_DEBUG("pcieCardRead:release MASK_WRITE_REQ suced.\n");
	}	
	//spin_unlock(&cardPrivateData->codeProtect);

	 up( &cardPrivateData->pcieDeviceSem);

	return count;
}

/*
	函数名：  pcieCardWrite
	参  数：
	返回值：
	函数功能：向PCIe设备上写入数据 
 * */
ssize_t pcieCardWrite( struct file * file, const char __user  * buff, size_t count, loff_t *f_ops)
{

	/* 获取板卡的私有数据结构 struct pcieCardPrivate * cardPrivateData */
	struct pcieCardPrivate * cardPrivateData = file->private_data;
	//unsigned int deviceCtlRegVal = 0;
	unsigned int DCSR3Val = 0;	
	/* test verible */
	unsigned int test_DMA_Addr;
	unsigned int test_DMA_CMD;
	unsigned int DMA_SIZE;
	//int crPrint = 0;
	PCIE_DEBUG("pcieCardWrite: enter pcieCardWrite fun.\n ");
	PCIE_DEBUG("pcieCardWrite: data lenth is %ld.\n", count);

	if ( count == 0) {
		printk("Write data to device failure.\n");
		return -1;
	}else if ( count > MRD_DMA_PKT_SIZE) {
		printk( " Write data too long to wtite to devce.\n");
		return -1;
	}
	/* 限定系统向设备发送的数据长读为 */
	// count = MRD_DMA_PKT_SIZE;
	if ( down_interruptible( &cardPrivateData->pcieDeviceSem)) {
		printk( "pcieDeviceSem opt error.\n");
		return 0;
	}
#if 0
	wait_event_interruptible( cardPrivateData->writeQueue, (atomic_read(&cardPrivateData->dmaRecvProcess) == 0));
	atomic_inc(&cardPrivateData->dmaRecvProcess);
#endif
	wait_event_interruptible( cardPrivateData->writeQueue, ( atomic_read( &cardPrivateData->writeResourceFree) > 0));
	/********************* DMA 方式传输**********************/
	PCIE_DEBUG( "pcieCardWrite: copy data form user by \"copy_from_user\"\n");
// test	
	//printk("PCIE:pcieCardWrite: count = %u, buff = %p, WR_writeResource->data = %p.\n",( unsigned)count,buff, cardPrivateData->WR_writeResource->data);
	// copy_from_user( cardPrivateData->dmaStreamVirAddrWrite, buff, count);
	copy_from_user( cardPrivateData->WR_writeResource->data, buff, count);

	atomic_dec( &cardPrivateData->writeResourceFree);
	cardPrivateData->WR_writeResource = cardPrivateData->WR_writeResource->next;


	if ( (atomic_read( &cardPrivateData->writeResourceFree) ==  (PKT_WRITE_RESOURCE_NUM-1) ) && ( /*atomic_read( &cardPrivateData->dmaRecvProcess) == 0*/\
													atomic_read( &cardPrivateData->dmaRecvProcess) == 0)) { 
		PCIE_DEBUG("********pcieCardWrite: first write data to device by DMA.\n");	
		if ( atomic_inc_return(&cardPrivateData->dmaRecvProcess) == 2) 
			goto eStart;
		//atomic_inc( &cardPrivateData->dmaRecvProcess);
		cardPrivateData->dmaAddrStreamRecv = pci_map_single( cardPrivateData->pdev, cardPrivateData->RD_writeResource->data, MRD_DMA_PKT_SIZE, PCI_DMA_TODEVICE);
		
		if ( cardPrivateData->dmaAddrStreamRecv == 0) {
			printk(" in pcieCardWrite \"pci_map_single\" failure.\n");
			return -1;
		}

		/* 将启动一次DMA传输的信息写至设备 */		
		iowrite32( cardPrivateData->dmaAddrStreamRecv, cardPrivateData->bar0MapedAddr + MRD_DMA_LOW_ADDR);
		test_DMA_Addr = ioread32( cardPrivateData->bar0MapedAddr + MRD_DMA_LOW_ADDR);
		PCIE_DEBUG( "pcieCardWrite: DMA Addr--> driver dma addr : %p, device dma addr: %x\n",( void *) cardPrivateData->dmaAddrStreamRecv, test_DMA_Addr);

		iowrite32( count, cardPrivateData->bar0MapedAddr + MRD_DMA_SIZE);
		DMA_SIZE = ioread32(cardPrivateData->bar0MapedAddr + MRD_DMA_SIZE);
		PCIE_DEBUG( "pcieCardWrite: DMA_SIZE--> driver count: %ld - device DMA_SIZE: %d.\n", count, DMA_SIZE);		

		DCSR3Val = ioread32( cardPrivateData->bar0MapedAddr + DCSR3);
		DCSR3Val |= DMA_READ_START; 
		iowrite32( DCSR3Val, cardPrivateData->bar0MapedAddr + DCSR3);
		test_DMA_CMD = ioread32( cardPrivateData->bar0MapedAddr + DCSR3);
		PCIE_DEBUG("pcieCardWrite: DMA CMD--> driver CMD: %x, devcie CMD: %x.\n", DCSR3Val, test_DMA_CMD);
	} 
	PCIE_DEBUG("pcieCardWrite: PCIe devcie driver take of a writeResource get data from user space.\n ");
	up( &cardPrivateData->pcieDeviceSem);
	return count;
eStart:
	atomic_dec( &cardPrivateData->dmaRecvProcess);
	PCIE_DEBUG("pcieCardWrite: PCIe devcie driver take of a writeResource get data from user space.\n ");
	up( &cardPrivateData->pcieDeviceSem);
	return count;
}

/*
	函数名：pcieCardIoctl
	参  数：
	返回值：
	函数功能：PCIe设备的I/O读写控制。
*/
long pcieCardIoctl( struct file * file, unsigned int cmd, unsigned long userInfo )
{

	struct ioctlUsrInfo * userInfoP = (struct ioctlUsrInfo *) userInfo;
	struct ioctlUsrInfo * infoFromUser;
	struct pcieCardPrivate * cardPrivateData = file->private_data;
	infoFromUser = kmalloc(sizeof( struct ioctlUsrInfo), GFP_KERNEL);
	PCIE_DEBUG("IOCTL:userInfoP:%p\n", userInfoP);
	copy_from_user( infoFromUser, userInfoP, sizeof( struct ioctlUsrInfo)); 

	printk("PCIe:IOCTL: regVali--%d, regSerial--%d.\n", infoFromUser->regVal, infoFromUser->regSerial);

	switch (cmd) {
	/* IO BAR Write */
	case WRITE_CMD: 
	//	printk("bar0MapedAddr%p,offset %d\n",cardPrivateData->bar0MapedAddr,cardPrivateData->writeSerial);
	//	printk("iowrite writeValue %ld\n",userInfo);
		// printk("bar0MapedAddr + 4 * serial :%p\n",cardPrivateData->bar0MapedAddr + ( 4 * cardPrivateData->writeSerial));
		// iowrite32( userInfo, cardPrivateData->bar0MapedAddr + (4 * cardPrivateData->writeSerial++));
		iowrite32( infoFromUser->regVal, cardPrivateData->bar0MapedAddr + infoFromUser->regSerial);
	 //	up( &cardPrivateData->pcieDeviceSem);
		return 0;
	/* IO BAR Read */
	case READ_CMD: 
	//	printk("bar0MapedAddr%p,offset %d\n",cardPrivateData->bar0MapedAddr,cardPrivateData->readSerial);
	//	printk("bar0MapedAddr + 4 * serial :%p\n",cardPrivateData->bar0MapedAddr + ( 4 * cardPrivateData->readSerial));
		// readValue = ioread32( cardPrivateData->bar0MapedAddr + (4 * cardPrivateData->readSerial++));
		PCIE_DEBUG("IOCTL: deal with--- READ_CMD\n");
		infoFromUser->regVal = ioread32( cardPrivateData->bar0MapedAddr + infoFromUser->regSerial);
		PCIE_DEBUG("IOCTL: infoFromUser->regVal:%X.\n", infoFromUser->regVal);
	 //	up( &cardPrivateData->pcieDeviceSem);
	//	printk("ioread readValue:%ld\n",readValue);
		return infoFromUser->regVal;
		
	case DEVICE_REST:
		pcieDeviceInit( cardPrivateData);		
		return 0;
	default:
	 //	up( &cardPrivateData->pcieDeviceSem);
		return -1;	
	}


}
/***************************************************************************************
 *	字符设备文件操作结构
 * *********************************************************************************** */

struct file_operations pcieCardFops = {
	.owner	=	THIS_MODULE,
	.read	=	pcieCardRead,
	.write	=	pcieCardWrite,
	.open	=	pcieCardOpen,
	.unlocked_ioctl	= pcieCardIoctl,
	.release=	pcieCardRelease,
};

/*
 *	函数名：pcieChrDevRegister
	参　数：
	返回值：
	函数功能：　实现一个字符设备的注册，注册字符设备用以控制PCIe设备的数据传输。
	
 * */

int pcieChrdevRegister( struct pci_dev *pdev, struct pcieCardPrivate * cardPrivateData)
{
	int err = 0;
	/*
	 *	pcie设备说有的信息都保存在struct pci_dev和 struct pcieCardPrivate结构体中，
	 * */
	/* 初始化设备读写控制信号量 */
	sema_init( &cardPrivateData->pcieDeviceSem, 1);
	sema_init( &cardPrivateData->intrMaskConfig, 0);
	
	/*
	 * 注册字符设备
	 * */
	err = alloc_chrdev_region( &cardPrivateData->chrDevNum, 0, 1, PCIE_DEVICE_NAME );
	if ( err) {
		printk(KERN_ERR"Alloc chardevice error.\n");
		return err;
	}
	cdev_init( &cardPrivateData->cdev, &pcieCardFops);
	cardPrivateData->cdev.owner = THIS_MODULE;

	cardPrivateData->cdev.ops = &pcieCardFops;
	err = cdev_add(&cardPrivateData->cdev, cardPrivateData->chrDevNum, 1);
	
	if ( err) {
		printk( KERN_ERR" Error adding /dev/chrdevPcie.\n");
		return err;
	}
	cardPrivateData->GbitPcieClass = class_create( THIS_MODULE, PCIE_DEVICE_NAME);
	device_create( cardPrivateData->GbitPcieClass, NULL, cardPrivateData->chrDevNum, NULL, PCIE_DEVICE_NAME);
	
#ifdef PIO_TEST
	/*
	 *	初始化cardPrivateData 中的recvBuffer\sendBuffer字段
	 * */
	cardPrivateData->recvBuffer = kmalloc( RECV_BUFFER_SIZE, GFP_KERNEL);
	if ( cardPrivateData->recvBuffer == NULL) {
		return -100;
	}
	cardPrivateData->sendBuffer = kmalloc( SEND_BUFFER_SIZE, GFP_KERNEL);
	if ( cardPrivateData->sendBuffer == NULL)  {
		kfree( cardPrivateData->recvBuffer);
		return -101;
	}
#endif
#ifdef CONSISTENT_DMA 
	/* 一致性ＤＭＡ映射，获得操作系统操作的虚拟地址　DMAsendBuffer,DMArecvBuffer　获得设备可操作的总线地址：damAddrSend dmaAddrRecv */
	cardPrivateData->DMAsendBuffer = pci_alloc_consistent( cardPrivateData->pdev, DMA_SEND_BUFFER_SIZE, &cardPrivateData->dmaAddrSend);
	if ( cardPrivateData->DMAsendBuffer == NULL) {
		printk(" pci_alloc_consistent sendBuffer error.\n");	
		return -102;
	}	
	cardPrivateData->DMArecvBuffer = pci_alloc_consistent( cardPrivateData->pdev, DMA_RECV_BUFFER_SIZE, &cardPrivateData->dmaAddrRecv);
	if ( cardPrivateData->DMArecvBuffer == NULL) {
		printk( "pci_alloc_consitent recvBuffer error.\n");
		return -103;
	}
#endif

#ifdef STREAM_DMA_TEST
	/* 开辟用流式ＤＭＡ映射的内存空间. */
	cardPrivateData->dmaStreamVirAddrWrite = kmalloc( SEND_BUFFER_SIZE, GFP_KERNEL);
	if ( cardPrivateData->dmaStreamVirAddrWrite == NULL) {
		printk( " kmalloc dmaStreamVirAddrWrite failure.\n");	
		return -104;
	}
	cardPrivateData->dmaStreamVirAddrRead = kmalloc( RECV_BUFFER_SIZE, GFP_KERNEL);
	if ( cardPrivateData->dmaStreamVirAddrRead == NULL) {
		printk( " kmalloc dmaStreamVirAddrRead failure.\n");
		return -105;
	}
#endif

	/* 创建多个用于流式ＤＭＡ映射的内存缓冲区，提高设备ＤＭＡ发送数据的效率。 */
	if (createReadResource(cardPrivateData) != PKT_READ_RESOURCE_NUM) {
		printk(" createReadResource failure.\n");
		return -106;
	}
	if ( createWriteResource( cardPrivateData) != PKT_WRITE_RESOURCE_NUM) {
		printk( " createWriteResource failure.\n");
		return -107;
	}
	/* 初始化 读写顺序变量 */
	cardPrivateData->readSerial = 0;
	cardPrivateData->writeSerial = 0;
	/* 初始化 读写函数中使用的临时地址变量 */
	cardPrivateData->tempBar0ReadAddr = NULL;
	cardPrivateData->tempBar0WriteAddr = NULL;
	/* 初始化接入到操作系统的PCIe设备 */
	err = pcieDeviceInit( cardPrivateData);
	if ( err) {
		printk(" pcieChrdevRegister --> pcieDeviceInit failure.\n");
		return -1;
	}
	return 0;
}
