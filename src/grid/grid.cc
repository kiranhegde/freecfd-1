/************************************************************************
	
	Copyright 2007-2010 Emre Sozer

	Contact: emresozer@freecfd.com

	This file is a part of Free CFD

	Free CFD is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    any later version.

    Free CFD is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    For a copy of the GNU General Public License,
    see <http://www.gnu.org/licenses/>.

*************************************************************************/
#include "grid.h"

string int2str(int number) ;

Grid::Grid() {
	// Just get the current processor's rank
	MPI_Comm_rank(MPI_COMM_WORLD, &Rank);
	// And total number of processors
	MPI_Comm_size(MPI_COMM_WORLD, &np);
}

void Grid::read(string fname, string format) {
	if (read_raw()) return;
	fstream file;
	fileName=fname;
	file.open(fileName.c_str());
	if (file.is_open()) {
		if (Rank==0) cout << "[I] Found grid file " << fileName  << endl;
		file.close();		
		if (format=="cgns") readCGNS();
		else if (format=="tecplot") readTEC();
		else {
			if (Rank==0) cerr << "[E] Unrecognized grid format: " << format << endl;
			exit(1);
		}
	} else {
		if (Rank==0) cerr << "[E] Grid file "<< fileName << " could not be found." << endl;
		exit(1);
	}
	return;
}

void Grid::setup(void) {
      if (Rank==0) cout << "[I] Partitioning the grid" << endl;
	partition();
      if (Rank==0) cout << "[I] Creating nodes and cells" << endl;
	create_nodes_cells();
	raw.node.clear();
      if (Rank==0) cout << "[I] Computing mesh dual" << endl;
	mesh2dual();
      if (Rank==0) cout << "[I] Creating faces" << endl;
	if (raw.type==CELL) create_faces();
	else if (raw.type==FACE) create_faces2();
      if (Rank==0) cout << "[I] Creating inter-partition ghost cells" << endl;
	create_partition_ghosts();
      if (Rank==0) cout << "[I] Computing output node id's" << endl;
	get_volume_output_ids();
	get_bc_output_ids();
      if (Rank==0) cout << "[I] Trimming memory" << endl;
	trim_memory();
      if (Rank==0) cout << "[I] Calculating areas and volumes" << endl;
	areas_volumes();
      if (Rank==0) cout << "[I] Creating boundary ghost cells" << endl;
	create_boundary_ghosts();
      if (Rank==0) cout << "[I] MPI handshake" << endl;
	mpi_handshake();
      if (Rank==0) cout << "[I] Getting ghost geometries" << endl;
	mpi_get_ghost_geometry();
	return;
}
	
void Grid::trim_memory() {
	// A trick for shrinking vector capacities to just the right sizes
	
	for (int n=0;n<nodeCount;++n) {
		vector<int> (node[n].cells).swap(node[n].cells);
		vector<int> (node[n].faces).swap(node[n].faces);
	}
	
	for (int f=0;f<faceCount;++f) { 
		vector<int> (face[f].nodes).swap(face[f].nodes);
	}

	for (int c=0;c<cellCount;++c) { 
		vector<int> (cell[c].nodes).swap(cell[c].nodes);
		vector<int> (cell[c].faces).swap(cell[c].faces);
		vector<int> (cell[c].neighborCells).swap(cell[c].neighborCells);
	}
	
	vector<Node> (node).swap(node);
	vector<Face> (face).swap(face);
	vector<Cell> (cell).swap(cell);
	
	vector<int> (partitionOffset).swap(partitionOffset);
		
	// Destroy grid raw data
	raw.node.clear();
	raw.cellConnIndex.clear();
	raw.cellConnectivity.clear();
	raw.bocoNodes.clear();
	raw.bocoNameMap.clear();
	raw.faceNodeCount.clear();
	raw.faceConnIndex.clear();
	raw.faceConnectivity.clear();
	raw.left.clear();
	raw.right.clear();
	
	return;
}

int Grid::areas_volumes() {
	Vec3D centroid;
	Vec3D areaVec;
	Vec3D patchCentroid,patchArea;
	Vec3D diagonal1,diagonal2;
	// Now loop through faces and calculate centroids and areas
	for (int f=0;f<faceCount;++f) {
		if (face[f].nodes.size()==4) { // Quad face
			diagonal1=faceNode(f,2)-faceNode(f,0);
			diagonal2=faceNode(f,3)-faceNode(f,1);
			face[f].normal=diagonal1.cross(diagonal2);
			face[f].area=0.5*fabs(face[f].normal);
			face[f].normal=face[f].normal.norm();
		} else {
			// Sum the area as a patch of triangles formed by connecting two nodes and an interior point
			areaVec=0.;
			int next;
			centroid=faceNode(f,0);
			for (int n=1;n<face[f].nodes.size();++n) {
				next=n+1;
				if (next==face[f].nodes.size()) next=0;
				patchArea=0.5*(faceNode(f,n)-centroid).cross(faceNode(f,next)-centroid);
				areaVec+=patchArea;
			}
			face[f].area=fabs(areaVec);
			face[f].normal=areaVec.norm();
		}
		face[f].centroid=0.;
		for (int n=0;n<face[f].nodes.size();++n) face[f].centroid+=faceNode(f,n);
		face[f].centroid/=double(face[f].nodes.size());
	}
	
	if (Rank==0) cout << "[I] Calculated face areas and centroids" << endl;
	
	// Loop through the cells and calculate the volumes
	double totalVolume=0.;
	for (int c=0;c<cellCount;++c) {
		// Calculate the cell centroid
		cell[c].centroid=0.;
		for (int cn=0;cn<cell[c].nodes.size();++cn) cell[c].centroid+=cellNode(c,cn);
		cell[c].centroid/=double(cell[c].nodes.size());
		cell[c].volume=0.;

		Vec3D height; 
		Vec3D base_area;
		Vec3D base_area_norm;
		for (int cf=0;cf<cell[c].faces.size();++cf) {
			int f=cell[c].faces[cf];
			base_area=face[f].area*face[f].normal;
			if (face[f].parent==c) base_area*=-1.;
			base_area_norm=base_area.norm();
			height=(cell[c].centroid-face[f].centroid).dot(base_area_norm)*base_area_norm;
			cell[c].volume+=base_area.dot(height)/3.;
		}
		totalVolume+=cell[c].volume;
	}
	globalTotalVolume=0.;
	MPI_Allreduce (&totalVolume,&globalTotalVolume,1,MPI_DOUBLE,MPI_SUM,MPI_COMM_WORLD);
	if (Rank==0) cout << "[I] Total Volume= " << globalTotalVolume << endl;
	
	int count=0;
	for (int f=0;f<faceCount;++f) {
		if (face[f].normal.dot(face[f].centroid-cell[face[f].parent].centroid)<0.) {
			count++;
			//cout << "[W Rank=" << Rank << "] Face " << f << " normal is pointing in to its parent cell ..." << endl;
			// Need to swap the face and reflect the area vector TODO Check if following is right
			// Never got to verify if the following works
			//face[f].normal*=-1.;
			//vector<int>::reverse_iterator rit;
			//face[f].nodes.assign(face[f].nodes.rbegin(),face[f].nodes.rend());
		}	    
	}
	
	if (count!=0) cout << "[W rank=" << Rank << "] " << count << " face normals out of " << faceCount << " are pointing in to parent cell" << endl;
	
	/* DEBUG
	if (count!=0) {
		for (int f=0;f<faceCount;++f) {
			if (face[f].normal.dot(face[f].centroid-cell[face[f].parent].centroid)<0.) {
				int c=face[f].parent;
				cout << c << "\t" << face[f].neighbor << endl;
				ofstream dfile;
				dfile.open("debug-cell.dat");
				dfile << "VARIABLES = \"x\", \"y\", \"z\"" << endl;
				dfile << "ZONE, T=\"cell\", ZONETYPE=FEBRICK, DATAPACKING=BLOCK" << endl;
				dfile << "NODES=" << cell[c].nodeCount << " , ELEMENTS=1" << endl;
				for (int i=0;i<3;++i) {
					for (int n=0;n<cell[c].nodeCount;++n) {
						dfile << cellNode(c,n)[i] << "\t";
					}
				}
				dfile << endl;
				for (int n=0;n<cell[c].nodeCount;++n) dfile << n+1 << "\t";
				dfile << endl;
				dfile.close();
				
				dfile.open("debug-face.dat");
				dfile << "VARIABLES = \"x\", \"y\", \"z\"" << endl;
				dfile << "ZONE, T=\"face\", ZONETYPE=FEQUADRILATERAL, DATAPACKING=BLOCK" << endl;
				dfile << "NODES=" << face[f].nodeCount << " , ELEMENTS=1" << endl;
				for (int i=0;i<3;++i) {
					for (int n=0;n<face[f].nodeCount;++n) {
						dfile << faceNode(f,n)[i] << "\t";
					}
				}
				dfile << endl;
				for (int n=0;n<face[f].nodeCount;++n) dfile << n+1 << "\t";
				dfile << endl;
				dfile.close();
				
				exit(1);
			}
		}
	}
	*/
	return 0;

}


Node::Node(double x, double y, double z) {
	comp[0]=x;
	comp[1]=y;
	comp[2]=z;
}

Cell::Cell(void) {
	;
}

Node& Grid::cellNode(int c, int n) {
	return node[cell[c].nodes[n]];
};

Face& Grid::cellFace(int c, int f) {
	return face[cell[c].faces[f]];
};

Node& Grid::faceNode(int f, int n) {
	return node[face[f].nodes[n]];
};

void Grid::mpi_handshake(void) {
	
	sendCells.resize(np);
	recvCells.resize(np);
	vector<int> sendCount (np,0);
	vector<int> recvCount (np,0); 
	recvCount.resize(np);
	
	
	for (int g=partition_ghosts_begin;g<=partition_ghosts_end;++g) {
		int p=cell[g].partition;
		recvCells[p].push_back(cell[g].globalId);
	}
	for (int p=0;p<np;++p) recvCount[p]=recvCells[p].size();
	
	// I know which cells to receive from each processor
	// I don't know which cells to send
	// I don't even know how many to send
	// First communicate to figure out which processor request how many ghost cell data
	
	MPI_Alltoall(&recvCount[0],1,MPI_INT,&sendCount[0],1,MPI_INT,MPI_COMM_WORLD);
	

	for (int p=0;p<np;++p) {
		sendCells[p].resize(sendCount[p]);
		MPI_Sendrecv(&recvCells[p][0],recvCount[p],MPI_INT,p,0,
					 &sendCells[p][0],sendCount[p],MPI_INT,p,0,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
	}

	// Commit MPI_VEC3D
	MPI_Datatype types[2]={MPI_INT,MPI_DOUBLE};
	int block_lengths[2];
	MPI_Aint displacements[2];

	
      for (int proc=0;proc<np;++proc) {
            for (int i=0;i<sendCells[proc].size();++i) sendCells[proc][i]=maps.cellGlobal2Local[sendCells[proc][i]];
            for (int i=0;i<recvCells[proc].size();++i) recvCells[proc][i]=maps.cellGlobal2Local[recvCells[proc][i]];
      }

	mpiGeomPack dummy4;
	displacements[0]=(long) &dummy4.ids[0] - (long) &dummy4;
	displacements[1]=(long) &dummy4.data[0] - (long) &dummy4;
	block_lengths[0]=2;
	block_lengths[1]=4;
	MPI_Type_create_struct(2,block_lengths,displacements,types,&MPI_GEOM_PACK);
	MPI_Type_commit(&MPI_GEOM_PACK);
	
}

void Grid::mpi_get_ghost_geometry(void) {
	
	for (int p=0;p<np;++p) {
		if (Rank!=p) {
			mpiGeomPack sendBuffer[sendCells[p].size()];
			mpiGeomPack recvBuffer[recvCells[p].size()];
			int id;
			for (int g=0;g<sendCells[p].size();++g) {
				id=sendCells[p][g];
				sendBuffer[g].ids[0]=myOffset+id;
				sendBuffer[g].ids[1]=id;
				for (int i=0;i<3;++i) sendBuffer[g].data[i]=cell[id].centroid[i];
				sendBuffer[g].data[3]=cell[id].volume;
			}
			
			MPI_Sendrecv(sendBuffer,sendCells[p].size(),MPI_GEOM_PACK,p,0,recvBuffer,recvCells[p].size(),MPI_GEOM_PACK,p,MPI_ANY_TAG,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
			
			for (int g=0;g<recvCells[p].size();++g) {
				id=recvCells[p][g];
				cell[id].matrix_id=recvBuffer[g].ids[0];
				cell[id].id_in_owner=recvBuffer[g].ids[1];
				for (int i=0;i<3;++i) cell[id].centroid[i]=recvBuffer[g].data[i];
				cell[g].volume=recvBuffer[g].data[3];
			}
		}
	}
	
	MPI_Barrier(MPI_COMM_WORLD);
	return;
} 

void Grid::write_raw(void) {
	
	ofstream file;
	int int_size=sizeof(int);
	int vec3d_size=sizeof(Vec3D);
	int bc_count=raw.bocoNameMap.size();
	int bc_no,conn_size,bocoNodes_size,string_size;
	string bc_name;
	char * bc_name_c;
	set<int>::iterator sit;
	map<string,int>::iterator mit;
	int boco_node;

	file.open("grid.raw",ios::out | ios::binary);
	
	file.write((char*) &globalNodeCount,int_size);
	file.write((char*) &raw.node[0],globalNodeCount*vec3d_size);
	
	file.write((char*) &globalCellCount,int_size);
	file.write((char*) &raw.cellConnIndex[0], raw.cellConnIndex.size()*int_size);
	
	conn_size=raw.cellConnectivity.size();
	file.write((char*) &conn_size,int_size);
	file.write((char*) &raw.cellConnectivity[0], raw.cellConnectivity.size()*int_size);
	
	bc_count=raw.bocoNameMap.size();
	file.write((char*) &bc_count,int_size);
	for (int b=0;b<bc_count;++b) {
		bocoNodes_size=raw.bocoNodes[b].size();
		file.write((char *) &bocoNodes_size,int_size);
		for (sit=raw.bocoNodes[b].begin();sit!=raw.bocoNodes[b].end();sit++) {
			boco_node=*sit;
			file.write((char*) &boco_node,int_size);
		}
	}
	for (mit=raw.bocoNameMap.begin();mit!=raw.bocoNameMap.end();mit++) {
		bc_name=(*mit).first;
		bc_no=(*mit).second;
		
		string_size=sizeof(char)*bc_name.size();
		// Write the size of the string
		file.write((char*) &string_size,int_size);
		file.write(bc_name.c_str(),string_size);
		file.write((char*) &bc_no,int_size);
	}
	
	file.close();
	
	cout << "\n[I] Wrote grid.raw file" << endl;
	
	return;
}
