
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <opencv2/opencv.hpp>

using namespace std;
using namespace cv;

class TileDims {
private:
    unordered_map<string, unordered_map<int, vector<int>>> tiles;
    unordered_map<string, unordered_map<int, vector<int>>> tilesForFGDetection;

public:
    TileDims() {
        ///////// For stitching ////////////////////////////////////////////
        // For video with resolution 1280x720
        unordered_map<int, vector<int>> dims;
        // Each value is a vector having x1, y1, x2, y2 of a tile indicated by the keys
        dims[0] = {0, 0, 320, 192};
        dims[1] = {320, 0, 640, 192};
        dims[2] = {640, 0, 960, 192};
        dims[3] = {960, 0, 1280, 192};
        dims[4] = {0, 192, 320, 384};
        dims[5] = {320, 192, 640, 384};
        dims[6] = {640, 192, 960, 384};
        dims[7] = {960, 192, 1280, 384};
        dims[8] = {0, 384, 320, 576};
        dims[9] = {320, 384, 640, 576};
        dims[10] = {640, 384, 960, 576};
        dims[11] = {960, 384, 1280, 576};
        dims[12] = {0, 576, 320, 720};
        dims[13] = {320, 576, 640, 720};
        dims[14] = {640, 576, 960, 720};
        dims[15] = {960, 576, 1280, 720};
        tiles["1280x720"] = dims;
    }

    vector<int> getTileDims(string video_resolution, int tile_index) {
        return tiles[video_resolution][tile_index];
    }
};