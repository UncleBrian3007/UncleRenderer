#pragma once

// Shared GPU-side structures mirroring Common.h from the Orochi Course.
// LeftIndex / RightIndex < 0  =>  leaf node (addr = ~index).

struct Node
{
    int LeftIndex;
    int RightIndex;
    int ParentAddr;
    int Pivot;
};

struct Leaf
{
    int Value;
    int ParentAddr;
};

bool NodeIsLeftLeaf(Node n)  { return n.LeftIndex  < 0; }
bool NodeIsRightLeaf(Node n) { return n.RightIndex < 0; }
int  NodeGetLeftAddr(Node n)  { return n.LeftIndex  < 0 ? ~n.LeftIndex  : n.LeftIndex;  }
int  NodeGetRightAddr(Node n) { return n.RightIndex < 0 ? ~n.RightIndex : n.RightIndex; }

bool IsLeaf(int nodeIndex)    { return nodeIndex < 0; }
int  GetNodeAddr(int nodeIndex) { return nodeIndex < 0 ? ~nodeIndex : nodeIndex; }
