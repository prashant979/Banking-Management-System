#include <stdio.h>

int main(void) {
    printf("Hello, World!\n");
    return 0;
}

// Execute Everytime
//sed -i 's/customer_write_all(fp, /customer_write_all(/' auth.c
//grep -n customer_write_all auth.c

/*
{ printf '%-6s,%-20s,%-20s,%-20s,%-20s,%s,%s\n' id firstname lastname username password a l
  printf '%-6d,%-20s,%-20s,%-20s,%-20s,%d,%d\n' 2001 Neha  Gupta neha  emp123 1 0
  printf '%-6d,%-20s,%-20s,%-20s,%-20s,%d,%d\n' 2002 Karan Verma karan emp456 1 0
} > employee.csv
wc -c employee.csv                    # must be 285 (95 x 3 rows)
awk 'length($0)!=94' employee.csv     # prints nothing if every row is the right width
*/

/*
LC_ALL=C awk -F, 'BEGIN{printf "%-10s,%-6s,%-6s,%-12s,%14s,%14s,%-12s,%s\n","txn_id","cust","peer","type","amount","balance_after","timestamp","s"}
NR>1 && NF>=6 && $1+0>0 {n++; printf "%-10d,%-6d,%-6d,%-12s,%14.2f,%14.2f,%-12d,%s\n", n,$2,0,$3,$4,$5,$6,"C"}' transaction.csv.bak > transaction.csv
*/

/*
LC_ALL=C awk 'BEGIN{printf "%-8s,%-6s,%-12s,%-200s\n","id","cust","timestamp","message"}
NR>1 { split($0,f,","); if (f[1]+0==0) next; msg=$0; sub(/^[^,]*,[^,]*,[^,]*,/,"",msg); n++;
       printf "%-8d,%-6d,%-12d,%-200s\n", n,f[2],f[3],substr(msg,1,200) }' feedback.csv.bak > feedback.csv
*/