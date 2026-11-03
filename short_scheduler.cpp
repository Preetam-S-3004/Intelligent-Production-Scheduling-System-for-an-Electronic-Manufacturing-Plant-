
#include <iostream>
#include <iomanip>
#include <cstring>
#include <cstdlib>
using namespace std;

const int TYPES = 5, MAX_M = 10, MAX_OPS = 5, MAX_ORD = 50, MAX_TIME = 400;
const char* TYPE_NAME[TYPES] = {"Assembly", "Soldering", "Testing", "Calibration", "Inspection"};
enum { PENDING, QUEUED, RUNNING, DONE };      // status of a step

// ---------------------------- data types -----------------------------------
struct Operation {                 // one step of an order
    int type, duration, dependsOn, status, start;   // dependsOn = -1 means none
};
struct Order {                     // one order = node of the linked list
    int id, priority, deadline, arrival;            // priority 1 = most important
    bool urgent, done;
    int opCount, completion;
    Operation ops[MAX_OPS];        // ARRAY of steps
    Order* next;
};
struct Machine {
    char name[20];
    int type, freeAt, downUntil, maintStart, maintEnd, curOp;
    bool busy;
    Order* curOrder;
};
struct Item {                      // a waiting step (queue node / array element)
    Order* order;
    int op;
    Item* next;
};
struct Record {                    // a history note (stack node)
    int orderId, op;
    Record* next;
};

// ---------------------------- STACK (last in, first out) --------------------
class Stack {
    Record* top; int count;
public:
    Stack() { top = NULL; count = 0; }
    ~Stack() { Record r; while (pop(r)) {} }
    void push(Record r) { Record* p = new Record(r); p->next = top; top = p; count++; }
    bool pop(Record& r) {
        if (!top) return false;
        Record* p = top; r = *p; top = p->next; delete p; count--;
        return true;
    }
    int size() { return count; }
};

// ---------------------------- QUEUE (first in, first out) -------------------
class Queue {
    Item *front, *rear;
public:
    Queue() { front = rear = NULL; }
    ~Queue() { Item x; while (dequeue(x)) {} }
    void enqueue(Item x) {                       // join the back
        Item* p = new Item(x); p->next = NULL;
        if (rear) rear->next = p; else front = p;
        rear = p;
    }
    void pushFront(Item x) {                     // join the front
        Item* p = new Item(x); p->next = front; front = p;
        if (!rear) rear = p;
    }
    bool dequeue(Item& x) {                      // leave from the front
        if (!front) return false;
        Item* p = front; x = *p; front = p->next;
        if (!front) rear = NULL;
        delete p;
        return true;
    }
};

// ---------------------------- LINKED LIST of orders -------------------------
class OrderList {
public:
    Order *head, *tail;
    OrderList() { head = tail = NULL; }
    ~OrderList() { while (head) { Order* p = head; head = head->next; delete p; } }
    void append(Order* o) {
        o->next = NULL;
        if (tail) tail->next = o; else head = o;
        tail = o;
    }
};

// ---------------------------- input data ------------------------------------
Order inputOrders[MAX_ORD];   int orderCount = 0;
Machine baseMachines[MAX_M];  int machineCount = 0;
struct Failure { int machine, time, duration; } fails[10];  int failCount = 0;
int gantt[MAX_M][MAX_TIME];   // 2D ARRAY: order number, -1 idle, -2 lost, -3 broken, -4 maintenance
bool disruptions = true;      // failures + maintenance ON/OFF

void addMachine(const char* name, int type, int maintStart, int maintEnd) {
    Machine& m = baseMachines[machineCount++];
    memset(&m, 0, sizeof(m));
    strcpy(m.name, name);
    m.type = type; m.maintStart = maintStart; m.maintEnd = maintEnd; m.curOp = -1;
}

// a0..a4 = hours for Assembly, Soldering, Testing, Calibration, Inspection (0 = skip)
void addOrder(int id, int pr, int dl, int arr, bool urgent, int a0, int a1, int a2, int a3, int a4) {
    Order& o = inputOrders[orderCount++];
    memset(&o, 0, sizeof(o));
    o.id = id; o.priority = pr; o.deadline = dl; o.arrival = arr; o.urgent = urgent;
    int hours[5] = {a0, a1, a2, a3, a4}, previous = -1;
    for (int k = 0; k < 5; k++) {
        if (hours[k] == 0) continue;
        Operation& op = o.ops[o.opCount];
        op.type = k; op.duration = hours[k]; op.dependsOn = previous; op.start = -1;
        previous = o.opCount++;
    }
}

void loadSampleData() {
    //          name           type maintenance(start,end)
    addMachine("Assembly-1",    0, 20, 26);
    addMachine("Assembly-2",    0, 0, 0);
    addMachine("Soldering-1",   1, 0, 0);
    addMachine("Soldering-2",   1, 0, 0);
    addMachine("Testing-1",     2, 0, 0);
    addMachine("Calibration-1", 3, 18, 22);
    addMachine("Inspection-1",  4, 0, 0);
    //         id  pr dl  arr urgent  Asm Sld Tst Cal Ins
    addOrder(101, 2, 40, 0,  false,   4,  3,  3,  2,  2);
    addOrder(102, 1, 30, 0,  false,   3,  4,  2,  0,  2);
    addOrder(103, 3, 60, 0,  false,   5,  3,  4,  3,  2);
    addOrder(104, 2, 45, 0,  false,   4,  2,  3,  2,  2);
    addOrder(105, 3, 70, 2,  false,   3,  3,  3,  0,  2);
    addOrder(106, 1, 35, 3,  false,   4,  3,  2,  2,  2);
    addOrder(107, 2, 55, 5,  false,   3,  4,  3,  2,  2);
    addOrder(108, 1, 28, 10, true,    2,  2,  2,  0,  1);   // URGENT
    addOrder(109, 3, 80, 12, false,   5,  4,  3,  3,  2);
    addOrder(110, 2, 60, 15, false,   3,  3,  4,  2,  2);
    // failures: machine number, time, repair hours
    fails[failCount++] = {2, 8, 6};     // Soldering-1 breaks at hour 8 for 6 hours
    fails[failCount++] = {4, 14, 5};    // Testing-1 breaks at hour 14 for 5 hours
}

// A loop means step A waits for B and B waits for A. We follow the chain
// with a STACK; if we walk more steps than the order has, there is a loop.
bool hasLoop(Order& o) {
    for (int i = 0; i < o.opCount; i++) {
        Stack path;
        for (int cur = i; cur != -1; cur = o.ops[cur].dependsOn) {
            Record r = {o.id, cur, NULL};
            path.push(r);
            if (path.size() > o.opCount) return true;
        }
    }
    return false;
}

bool checkInput() {
    bool ok = true, haveMachine[TYPES] = {false};
    for (int i = 0; i < machineCount; i++) haveMachine[baseMachines[i].type] = true;
    for (int k = 0; k < orderCount; k++) {
        Order& o = inputOrders[k];
        for (int i = 0; i < o.opCount; i++)
            if (!haveMachine[o.ops[i].type]) {
                cout << "Error: no machine for " << TYPE_NAME[o.ops[i].type] << "\n";
                ok = false;
            }
        if (hasLoop(o)) { cout << "Error: order " << o.id << " has a dependency loop\n"; ok = false; }
    }
    return ok;
}

// ---------------------------- SORTING (CO3) ---------------------------------
// Should step a go before step b?  urgent > priority > deadline > shorter step
bool goesBefore(Item a, Item b) {
    Order *x = a.order, *y = b.order;
    if (x->urgent != y->urgent) return x->urgent;
    if (x->priority != y->priority) return x->priority < y->priority;
    if (x->deadline != y->deadline) return x->deadline < y->deadline;
    return x->ops[a.op].duration < y->ops[b.op].duration;
}

void insertionSort(Item list[], int n) {          // used by the scheduler
    for (int i = 1; i < n; i++) {
        Item key = list[i];
        int j = i - 1;
        while (j >= 0 && goesBefore(key, list[j])) { list[j + 1] = list[j]; j--; }
        list[j + 1] = key;
    }
}

bool machineIsFree(Machine& m, int t) {
    return !m.busy && t >= m.downUntil && !(t >= m.maintStart && t < m.maintEnd);
}

// ---------------------------- results ---------------------------------------
struct Result {
    int makespan, delayed, totalLate, stopped, notFinished, idleTotal, n;
    int idle[MAX_M], id[MAX_ORD], deadline[MAX_ORD], finish[MAX_ORD], late[MAX_ORD];
    double avgTurnaround;
};

// ---------------------------- THE SCHEDULER ---------------------------------
// smart = true : urgent queues + sorting.   smart = false : first come first served.
Result runSchedule(bool smart, bool useDisruptions) {
    Result R; memset(&R, 0, sizeof(R));
    Machine m[MAX_M];
    for (int i = 0; i < machineCount; i++) {
        m[i] = baseMachines[i];
        if (!useDisruptions) m[i].maintStart = m[i].maintEnd = 0;
    }
    for (int i = 0; i < MAX_M; i++) for (int t = 0; t < MAX_TIME; t++) gantt[i][t] = -1;

    OrderList master;                       // linked list of arrived orders
    Queue normalQ[TYPES], urgentQ[TYPES];   // waiting lines, one pair per step type
    Stack history;                          // history of started steps
    int finished = 0, t;

    for (t = 0; t < MAX_TIME && finished < orderCount; t++) {

        // 1. finish steps that end now
        for (int i = 0; i < machineCount; i++) {
            if (!(m[i].busy && m[i].freeAt == t)) continue;
            Order* o = m[i].curOrder;
            o->ops[m[i].curOp].status = DONE;
            m[i].busy = false;
            bool all = true;
            for (int k = 0; k < o->opCount; k++) if (o->ops[k].status != DONE) all = false;
            if (all) { o->done = true; o->completion = t; finished++; }
        }

        // 2. machine failures (undo the stopped step using the STACK)
        if (useDisruptions)
            for (int e = 0; e < failCount; e++) {
                if (fails[e].time != t) continue;
                int i = fails[e].machine;
                m[i].downUntil = t + fails[e].duration;
                if (!m[i].busy) continue;
                Order* o = m[i].curOrder;
                int k = m[i].curOp;
                for (int s = o->ops[k].start; s < t; s++) gantt[i][s] = -2;   // work lost
                Stack temp; Record r;
                while (history.pop(r)) {                    // find this step's note
                    if (r.orderId == o->id && r.op == k) break;
                    temp.push(r);                           // keep the others aside
                }
                while (temp.pop(r)) history.push(r);        // put the others back
                o->ops[k].status = QUEUED;
                Item x = {o, k, NULL};
                urgentQ[o->ops[k].type].pushFront(x);       // restart it first
                m[i].busy = false;
                R.stopped++;
            }

        // 3. new orders arrive: add to the linked list
        for (int k = 0; k < orderCount; k++)
            if (inputOrders[k].arrival == t) {
                Order* o = new Order(inputOrders[k]);
                master.append(o);
            }

        // 4. find steps that are ready (the step before is DONE)
        Item ready[MAX_ORD * MAX_OPS];
        int readyCount = 0;
        for (Order* o = master.head; o; o = o->next) {
            if (o->done) continue;
            for (int k = 0; k < o->opCount; k++) {
                Operation& op = o->ops[k];
                if (op.status == PENDING && (op.dependsOn == -1 || o->ops[op.dependsOn].status == DONE))
                    ready[readyCount++] = Item{o, k, NULL};
            }
        }

        // 5. sort the ready steps and put them in the queues
        if (smart) insertionSort(ready, readyCount);
        for (int i = 0; i < readyCount; i++) {
            Order* o = ready[i].order;
            o->ops[ready[i].op].status = QUEUED;
            int type = o->ops[ready[i].op].type;
            if (smart && o->urgent) urgentQ[type].enqueue(ready[i]);
            else normalQ[type].enqueue(ready[i]);
        }

        // 6. give waiting steps to free machines (linear search over machines)
        for (int i = 0; i < machineCount; i++) {
            if (!machineIsFree(m[i], t)) continue;
            Item x;
            if (!urgentQ[m[i].type].dequeue(x) && !normalQ[m[i].type].dequeue(x)) continue;
            Operation& op = x.order->ops[x.op];
            op.status = RUNNING; op.start = t;
            m[i].busy = true; m[i].freeAt = t + op.duration;
            m[i].curOrder = x.order; m[i].curOp = x.op;
            Record r = {x.order->id, x.op, NULL};
            history.push(r);
        }

        // 7. write this hour in the Gantt table
        for (int i = 0; i < machineCount; i++)
            gantt[i][t] = m[i].busy ? m[i].curOrder->id
                        : (t < m[i].downUntil) ? -3
                        : (t >= m[i].maintStart && t < m[i].maintEnd) ? -4 : -1;
    }

    // results
    double turnSum = 0; int doneCount = 0;
    for (Order* o = master.head; o; o = o->next, R.n++) {
        R.id[R.n] = o->id; R.deadline[R.n] = o->deadline;
        if (!o->done) { R.finish[R.n] = -1; R.notFinished++; continue; }
        R.finish[R.n] = o->completion;
        R.late[R.n] = max(0, o->completion - o->deadline);
        if (R.late[R.n] > 0) R.delayed++;
        R.totalLate += R.late[R.n];
        R.makespan = max(R.makespan, o->completion);
        turnSum += o->completion - o->arrival;
        doneCount++;
    }
    if (doneCount) R.avgTurnaround = turnSum / doneCount;
    for (int i = 0; i < machineCount; i++) {
        m[i].freeAt = 0;
        for (int s = 0; s < R.makespan; s++) if (gantt[i][s] == -1) R.idle[i]++;
        R.idleTotal += R.idle[i];
    }
    return R;
}

// ---------------------------- printing --------------------------------------
void printGantt(Result& R) {
    cout << "\nGANTT CHART (hours)  number = order running, . = idle, X = work lost,"
         << " # = machine broken, M = maintenance\n";
    for (int start = 0; start < R.makespan; start += 25) {
        int end = min(R.makespan, start + 25);
        cout << "\nHour          ";
        for (int t = start; t < end; t++) cout << setw(3) << t << " ";
        cout << "\n";
        for (int i = 0; i < machineCount; i++) {
            cout << left << setw(14) << baseMachines[i].name << right;
            for (int t = start; t < end; t++) {
                int v = gantt[i][t];
                if (v >= 0) cout << setw(3) << v;
                else cout << setw(3) << (v == -1 ? "." : v == -2 ? "X" : v == -3 ? "#" : "M");
                cout << " ";
            }
            cout << "\n";
        }
    }
    cout << "\n";
}

void printOrders(Result& R) {
    cout << "ORDER RESULTS\n" << left << setw(8) << "Order" << setw(18) << "Deadline (hour)"
         << setw(18) << "Finished (hour)" << setw(14) << "Late (hours)" << "Status\n";
    for (int i = 0; i < R.n; i++) {
        cout << setw(8) << R.id[i] << setw(18) << R.deadline[i];
        if (R.finish[i] < 0) cout << setw(18) << "-" << setw(14) << "-" << "NOT FINISHED\n";
        else cout << setw(18) << R.finish[i] << setw(14) << R.late[i] << (R.late[i] ? "LATE" : "On time") << "\n";
    }
    cout << right;
}

void printMetrics(const char* title, Result& R) {
    cout << "\n===== " << title << " =====\n";
    cout << "Total time               = " << R.makespan << " hours\n";
    cout << "Delayed orders           = " << R.delayed << " of " << R.n << "\n";
    cout << "Total lateness           = " << R.totalLate << " hours\n";
    cout << "Average turnaround       = " << fixed << setprecision(2) << R.avgTurnaround << " hours\n";
    cout << "Steps stopped by failure = " << R.stopped << "\n";
    cout << "Machine idle time (all)  = " << R.idleTotal << " hours\n";
    cout << "Idle time of each machine:\n";
    for (int i = 0; i < machineCount; i++)
        cout << "   " << left << setw(14) << baseMachines[i].name << right << "= " << R.idle[i] << " hours\n";
    if (R.notFinished) cout << "WARNING: " << R.notFinished << " order(s) did not finish.\n";
}

void compareBoth() {
    Result a = runSchedule(true, disruptions), b = runSchedule(false, disruptions);
    cout << "\n===== SMART SCHEDULER vs FIRST COME FIRST SERVED =====\n";
    cout << "Failures and maintenance: " << (disruptions ? "ON" : "OFF") << "\n\n"
         << left << setw(28) << "Measure" << setw(10) << "Smart" << "First come first served\n"
         << "------------------------------------------------------\n"
         << setw(28) << "Total time (hours)" << setw(10) << a.makespan << b.makespan << "\n"
         << setw(28) << "Delayed orders" << setw(10) << a.delayed << b.delayed << "\n"
         << setw(28) << "Total lateness (hours)" << setw(10) << a.totalLate << b.totalLate << "\n"
         << setw(28) << "Machine idle time (hours)" << setw(10) << a.idleTotal << b.idleTotal << "\n"
         << setw(28) << "Steps stopped by failure" << setw(10) << a.stopped << b.stopped << "\n"
         << setw(28) << "Average turnaround (hours)" << setw(10) << fixed << setprecision(2)
         << a.avgTurnaround << b.avgTurnaround << "\n\nLateness of each order (hours):\n"
         << setw(10) << "Order" << setw(10) << "Smart" << "First come first served\n";
    for (int i = 0; i < a.n; i++)
        cout << setw(10) << a.id[i] << setw(10) << a.late[i] << b.late[i] << "\n";
    cout << right;
}

// ---------------------------- sorting and searching demo --------------------
long long counter = 0;      // counts comparisons

void bubbleSort(int a[], int n) {
    for (int i = 0; i < n - 1; i++)
        for (int j = 0; j < n - 1 - i; j++) {
            counter++;
            if (a[j] > a[j + 1]) swap(a[j], a[j + 1]);
        }
}

void mergeSort(int a[], int tmp[], int l, int r) {      // split, sort halves, join
    if (l >= r) return;
    int mid = (l + r) / 2, i = l, j = mid + 1, k = l;
    mergeSort(a, tmp, l, mid);
    mergeSort(a, tmp, mid + 1, r);
    while (i <= mid && j <= r) { counter++; tmp[k++] = (a[i] <= a[j]) ? a[i++] : a[j++]; }
    while (i <= mid) tmp[k++] = a[i++];
    while (j <= r) tmp[k++] = a[j++];
    for (k = l; k <= r; k++) a[k] = tmp[k];
}

int linearSearch(int a[], int n, int key, int& steps) {
    for (steps = 0; steps < n; ) if (a[steps++] == key) return steps - 1;
    return -1;
}

int binarySearch(int a[], int n, int key, int& steps) {       // array must be sorted
    int low = 0, high = n - 1; steps = 0;
    while (low <= high) {
        int mid = (low + high) / 2;
        steps++;
        if (a[mid] == key) return mid;
        if (a[mid] < key) low = mid + 1; else high = mid - 1;
    }
    return -1;
}

void sortingDemo() {
    const int N = 1000;
    static int original[N], work[N], tmp[N];
    srand(42);
    for (int i = 0; i < N; i++) original[i] = rand() % 100000;

    memcpy(work, original, sizeof(original)); counter = 0; bubbleSort(work, N);
    cout << "\nSorting " << N << " numbers:\n  Bubble sort : " << counter << " comparisons  O(n^2)\n";
    memcpy(work, original, sizeof(original)); counter = 0; mergeSort(work, tmp, 0, N - 1);
    cout << "  Merge sort  : " << counter << " comparisons  O(n log n)\n";

    int key = work[N - 1], s1, s2;               // work[] is sorted now
    linearSearch(original, N, key, s1);
    binarySearch(work, N, key, s2);
    cout << "\nSearching for " << key << ":\n  Linear search : " << s1 << " checks  O(n)\n"
         << "  Binary search : " << s2 << " checks  O(log n)\n";
}

void showData() {
    cout << "MACHINES"<<endl;
    for (int i = 0; i < machineCount; i++) {
        cout << "  " << left << setw(14) << baseMachines[i].name << right << TYPE_NAME[baseMachines[i].type];
        if (baseMachines[i].maintEnd) cout << "  (maintenance hour " << baseMachines[i].maintStart
                                           << " to " << baseMachines[i].maintEnd - 1 << ")";
        cout << "\n";
    }
    cout << "\nORDERS (steps shown as first letter : hours)\n";
    for (int i = 0; i < orderCount; i++) {
        Order& o = inputOrders[i];
        cout << "  " << o.id << "  priority " << o.priority << "  deadline " << setw(2) << o.deadline
             << "  arrives " << setw(2) << o.arrival << (o.urgent ? "  URGENT" : "        ") << "  ";
        for (int k = 0; k < o.opCount; k++) cout << TYPE_NAME[o.ops[k].type][0] << ":" << o.ops[k].duration << " ";
        cout << "\n";
    }
    cout << "\nFAILURES\n";
    for (int e = 0; e < failCount; e++)
        cout << "  " << baseMachines[fails[e].machine].name << " breaks at hour " << fails[e].time
             << " for " << fails[e].duration << " hours\n";
}

// ---------------------------- main menu -------------------------------------
int main() {
    loadSampleData();
    if (!checkInput()) return 1;
    int choice = -1;
    while (choice != 0) {
        cout << "\n====== PRODUCTION SCHEDULING SYSTEM ======\n"
             << " 1. Run smart scheduler (priority + deadline)\n"
             << " 2. Run first come first served scheduler\n"
             << " 3. Compare both\n"
             << " 4. Sorting and searching test\n"
             << " 5. Show input data\n"
             << " 6. Failures and maintenance ON/OFF (now " << (disruptions ? "ON" : "OFF") << ")\n"
             << " 0. Exit\nYour choice: ";
        if (!(cin >> choice)) { cin.clear(); cin.ignore(10000, '\n'); choice = -1; }

        if (choice == 1 || choice == 2) {
            Result R = runSchedule(choice == 1, disruptions);
            printGantt(R);
            printOrders(R);
            printMetrics(choice == 1 ? "SMART SCHEDULER" : "FIRST COME FIRST SERVED", R);
        }
        else if (choice == 3) compareBoth();
        else if (choice == 4) sortingDemo();
        else if (choice == 5) showData();
        else if (choice == 6) disruptions = !disruptions;
        else if (choice != 0) cout << "Wrong choice. Try again.\n";
    }
    cout << "Bye."<<endl;
    return 0;
}
