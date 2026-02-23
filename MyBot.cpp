#include "hlt/game.hpp"
#include "hlt/constants.hpp"
#include "hlt/log.hpp"
#include <algorithm>
#include <random>
#include <set>
#include <map>
using namespace std;
using namespace hlt;

int main(int argc, char* argv[]) {
    // Initialisation du générateur aléatoire (soit via l'argument, soit via l'heure système)
    unsigned int rng_seed;
    if (argc > 1) {
        rng_seed = static_cast<unsigned int>(stoul(argv[1]));
    }
    else {
        rng_seed = static_cast<unsigned int>(time(nullptr));
    }
    mt19937 rng(rng_seed);

    Game game;
    game.ready("CigmanIA");

    // On garde en mémoire si un vaisseau est en mode "retour à la base" ou non
    map<EntityId, bool> is_returning;

    for (;;) {
        game.update_frame();
        shared_ptr<Player> me = game.me;
        unique_ptr<GameMap>& game_map = game.game_map;
        int map_width = game_map->width;

        vector<Command> command_queue;
        set<int> occupied_indices; // Pour éviter que deux vaisseaux ne visent la même case ce tour-ci

        // On force le retour un peu avant la fin pour déposer le dernier chargement
        bool end_game_mode = game.turn_number > constants::MAX_TURNS - 30;

        // Récupération des vaisseaux
        // La map par défaut n'est pas triable comme on veut, donc on copie tout dans un vecteur.
        vector<shared_ptr<Ship>> my_ships;
        for (const auto& kv : me->ships) {
            my_ships.push_back(kv.second);
        }

        // Mise à jour des états
        for (auto& ship : my_ships) {
            EntityId id = ship->id;
            // Initialisation de l'état si c'est un nouveau vaisseau
            if (is_returning.find(id) == is_returning.end()) 
                is_returning[id] = false;

            if (end_game_mode) {
                is_returning[id] = true; // En fin de partie tous les bateaux rentres
            }
            else {
                if (is_returning[id]) {
                    // Quand le bateau est au shipyard alors il repasse en mode minage
                    if (ship->position == me->shipyard->position) is_returning[id] = false;
                }
                else {
                    // le bateau rentre quand il est a 90% de sa charge max
                    if (ship->halite >= constants::MAX_HALITE * 0.90) is_returning[id] = true;
                }
            }
        }

        // Trie des vaisseaux pour décider qui bouge en premier
        std::sort(my_ships.begin(), my_ships.end(), [&](const shared_ptr<Ship>& a, const shared_ptr<Ship>& b) {
            bool a_ret = is_returning[a->id];
            bool b_ret = is_returning[b->id];

            if (a_ret != b_ret) return a_ret > b_ret;
            return a->halite > b->halite;
            });

        // Boucle de décision pour chaque vaisseau
        for (const auto& ship : my_ships) {
            EntityId id = ship->id;

            int halite_on_cell = game_map->at(ship)->halite;
            int move_cost = halite_on_cell / constants::MOVE_COST_RATIO;

            // On vérifie d'abord si on a assez de carburant pour bouger
            bool can_move = (ship->halite >= move_cost);

            Position target_pos = ship->position;
            bool force_move = false;

            if (!can_move) {
                target_pos = ship->position; // Cas ou on ne peut pas bouger
            }
            else if (is_returning[id]) {
                target_pos = me->shipyard->position; // On retourne au shipyard
            }
            else {
                // Gain si on reste (25% du sol)
                double gain_stay = halite_on_cell * 0.25;

                // Scan des cases voisines
                double max_gain_move = -1.0;
                Position best_neighbor = ship->position;

                for (const auto& dir : ALL_CARDINALS) {
                    Position p = ship->position.directional_offset(dir);
                    // Le gain réel du mouvement, c'est ce qu'on récolte moins ce qu'on a payé pour y aller
                    double gain = (game_map->at(p)->halite * 0.25) - move_cost;

                    if (gain > max_gain_move) {
                        max_gain_move = gain;
                        best_neighbor = p;
                    }
                }

                // Ajout d'un biais sur la case actuelle pour etre sur que le deplacement est rentable
                if (max_gain_move > gain_stay * 1.2) {
                    target_pos = best_neighbor;
                }
                else {
                    target_pos = ship->position; // On continue de miner ici
                }
            }

            // Pathfinding simple
            Direction best_move = Direction::STILL;

            // Cas A : On reste
            if (target_pos == ship->position) {
                best_move = Direction::STILL;

                // on en reste pas sur le shipyard
                if (ship->position == me->shipyard->position && !end_game_mode && can_move) {
                    int best_escape_halite = -1;

                    for (auto d : ALL_CARDINALS) {
                        Position p = ship->position.directional_offset(d);
                        int p_idx = p.y * map_width + p.x;

                        // On vérifie qu'on ne fonce pas dans quelqu'un
                        if (!occupied_indices.count(p_idx) && !game_map->at(p)->is_occupied()) {
                            if (game_map->at(p)->halite > best_escape_halite) {
                                best_escape_halite = game_map->at(p)->halite;
                                best_move = d;
                            }
                        }
                    }
                }
            }
            // Cas B : On veut se deplacer vers une cible
            else {
                if (can_move) {
                    // Pas de A* car trop couteux, on regarde juste les directions qui nous rapprochent de la cible
                    int target_dist = game_map->calculate_distance(ship->position, target_pos);
                    vector<Direction> valid_moves;

                    for (const auto& dir : ALL_CARDINALS) {
                        Position p = ship->position.directional_offset(dir);
                        int dist = game_map->calculate_distance(p, target_pos);
                        int p_idx = p.y * map_width + p.x;

                        // Vérification des collisions
                        bool blocked = occupied_indices.count(p_idx) || game_map->at(p)->is_occupied();

                        if (!blocked) {
                            if (dist < target_dist) {
                                // Cas ou la distance est reduite
                                valid_moves.push_back(dir);
                            }
                            // Cas ou on mine
                            else if (!is_returning[id] && p == target_pos) {
                                valid_moves.push_back(dir);
                            }
                        }
                    }

                    // S'il y a plusieurs options, on prend la première dispo.
                    if (!valid_moves.empty())
                        best_move = valid_moves[0];
                }
            }

            if (best_move != Direction::STILL && ship->halite < move_cost) 
                best_move = Direction::STILL;

            Position final_pos = ship->position.directional_offset(best_move);
            int final_idx = final_pos.y * map_width + final_pos.x;

            if (occupied_indices.count(final_idx)) {
                best_move = Direction::STILL;
                final_pos = ship->position;
                final_idx = final_pos.y * map_width + final_pos.x;
            }

            // On marque la case comme occupée pour les vaisseaux suivants
            occupied_indices.insert(final_idx);

            if (best_move == Direction::STILL) 
                command_queue.push_back(ship->stay_still());
            else 
                command_queue.push_back(ship->move(best_move));
        }

        int shipyard_idx = me->shipyard->position.y * map_width + me->shipyard->position.x;

        // On arrête de produire 80 tours avant la fin
        if (game.turn_number <= constants::MAX_TURNS - 80 &&
            me->halite >= constants::SHIP_COST &&
            !occupied_indices.count(shipyard_idx) &&
            !game_map->at(me->shipyard)->is_occupied())
        {
            command_queue.push_back(me->shipyard->spawn());
        }

        if (!game.end_turn(command_queue)) 
            break;
    }
    return 0;
}