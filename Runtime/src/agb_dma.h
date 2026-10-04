/* Runtime-internal DMA state. */
#ifndef AGB_DMA_H
#define AGB_DMA_H

typedef struct AgbDmaChannel
{
    const void *src;
    void *dst;
    void *reload;
    unsigned int control;
} AgbDmaChannel;

extern AgbDmaChannel agb_dma[4];

void agb_dma_transfer(AgbDmaChannel *ch, unsigned int count);
/* timing: 1 = VBlank, 2 = HBlank */
void agb_dma_timing(int timing);

#endif
